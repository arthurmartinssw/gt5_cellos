// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_fs.cpp
#include "lv2/sys_fs.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <system_error>

#include "common/log.h"
#include "ppu/ppu_code_cache.h"

namespace cell::lv2 {

namespace {

namespace fs = std::filesystem;

s32 FromFileError(host::FileError e) {
    switch (e) {
    case host::FileError::None: return CELL_OK;
    case host::FileError::NotFound: return CELL_ENOENT;
    case host::FileError::Exists: return CELL_EEXIST;
    case host::FileError::Access: return CELL_EACCES;
    case host::FileError::IsDir: return CELL_EISDIR;
    case host::FileError::NotDir: return CELL_ENOTDIR;
    case host::FileError::NoSpace: return CELL_ENOSPC;
    case host::FileError::TooMany: return CELL_EMFILE;
    case host::FileError::NameTooLong: return CELL_ENAMETOOLONG;
    case host::FileError::Io: return CELL_EIO;
    }
    return CELL_EIO;
}

s32 FromErrorCode(const std::error_code& ec) {
    if (ec == std::errc::no_such_file_or_directory) return CELL_ENOENT;
    if (ec == std::errc::file_exists) return CELL_EEXIST;
    if (ec == std::errc::directory_not_empty) return CELL_ENOTEMPTY;
    if (ec == std::errc::not_a_directory) return CELL_ENOTDIR;
    if (ec == std::errc::is_a_directory) return CELL_EISDIR;
    if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) return CELL_EACCES;
    if (ec == std::errc::no_space_on_device) return CELL_ENOSPC;
    if (ec == std::errc::filename_too_long) return CELL_ENAMETOOLONG;
    if (ec == std::errc::cross_device_link) return CELL_EXDEV;
    if (ec == std::errc::read_only_file_system) return CELL_EROFS;
    return CELL_EIO;
}

// file_time_type -> Unix seconds without relying on clock_cast (not in every C++20 library).
s64 UnixTime(fs::file_time_type t) {
    using namespace std::chrono;
    const auto sys = time_point_cast<system_clock::duration>(t - fs::file_time_type::clock::now() + system_clock::now());
    return duration_cast<seconds>(sys.time_since_epoch()).count();
}

bool IsBdvd(const VfsMount* m) { return m && m->prefix == "/dev_bdvd"; }
u64 SectorSize(const VfsMount* m) { return IsBdvd(m) ? 2048 : 512; }
u64 BlockSize(const VfsMount* m) { return IsBdvd(m) ? 2048 : 4096; }

const char* AccessName(s32 flags) {
    switch (flags & CELL_FS_O_ACCMODE) {
    case CELL_FS_O_RDONLY: return "r";
    case CELL_FS_O_WRONLY: return "w";
    default: return "rw";
    }
}

}  // namespace

void WriteFsStat(vm::GuestMemory& m, u32 ea, const FsStat& st) {
    m.Write<u32>(ea + 0, static_cast<u32>(st.mode));
    m.Write<u32>(ea + 4, static_cast<u32>(st.uid));
    m.Write<u32>(ea + 8, static_cast<u32>(st.gid));
    m.Write<u64>(ea + 12, static_cast<u64>(st.atime));
    m.Write<u64>(ea + 20, static_cast<u64>(st.mtime));
    m.Write<u64>(ea + 28, static_cast<u64>(st.ctime));
    m.Write<u64>(ea + 36, st.size);
    m.Write<u64>(ea + 44, st.blksize);
}

void WriteFsDirent(vm::GuestMemory& m, u32 ea, const FsDirent& d) {
    const std::size_t len = std::min<std::size_t>(d.name.size(), kFsMaxName);
    m.Fill(ea, 0, kFsDirentSize);
    m.Write<u8>(ea + 0, d.type);
    m.Write<u8>(ea + 1, static_cast<u8>(len));
    m.CopyIn(ea + 2, d.name.data(), len);
}

FsService::~FsService() = default;

// ---------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------

s32 FsService::Resolve(std::string_view path, VfsResolved* out) const {
    if (path.empty() || path.front() != '/') return CELL_ENOENT;
    if (path.size() > kFsMaxPath) return CELL_ENAMETOOLONG;
    // ".." can never leave a mount (the VFS refuses it); report it as a missing path.
    if (path.find("/../") != std::string_view::npos || path.ends_with("/..")) return CELL_ENOENT;
    auto r = kernel_.vfs().ResolveExisting(path);
    if (!r) return CELL_ENOTMOUNTED;  // no device mounted at that prefix (/dev_usb000...)
    *out = std::move(*r);
    return CELL_OK;
}

s32 FsService::ReadPath(u32 ea, std::string* out) const {
    if (!ea) return CELL_EFAULT;
    auto s = kernel_.memory().ReadCString(ea, kFsMaxPath + 1);
    if (!s) return CELL_EFAULT;
    if (s->size() > kFsMaxPath) return CELL_ENAMETOOLONG;
    *out = std::move(*s);
    return CELL_OK;
}

std::shared_ptr<FsHandle> FsService::Get(u32 fd, FsHandle::Kind kind) const {
    if (fd < kFsFirstFd || fd >= kFsFdLimit) return nullptr;
    std::lock_guard lock(mutex_);
    const auto& h = fds_[fd];
    return h && h->kind == kind ? h : nullptr;
}

s32 FsService::Insert(std::shared_ptr<FsHandle> handle, u32* fd) {
    std::lock_guard lock(mutex_);
    for (u32 i = kFsFirstFd; i < kFsFdLimit; ++i)
        if (!fds_[i]) {
            fds_[i] = std::move(handle);
            *fd = i;
            return CELL_OK;
        }
    return CELL_EMFILE;
}

void FsService::NoteMissing(std::string_view path) {
    std::lock_guard lock(mutex_);
    std::erase(missing_, std::string(path));
    missing_.emplace_back(path);
    if (missing_.size() > 16) missing_.pop_front();
}

void FsService::InvalidateCode(u32 ea, u64 size) {
    if (!size) return;
    vm::GuestMemory& m = kernel_.memory();
    for (u64 p = ea >> vm::kPageShift; p <= (u64{ea} + size - 1) >> vm::kPageShift; ++p)
        if (m.Flags(static_cast<u32>(p << vm::kPageShift)) & vm::kPageCode) {
            kernel_.code().Invalidate(ea, size);  // data read over translated code (overlays)
            return;
        }
}

FsStat FsService::StatOf(const fs::path& host, const VfsMount* mount, s32* error) const {
    FsStat st;
    std::error_code ec;
    const fs::file_status s = fs::status(host, ec);
    if (ec || !fs::exists(s)) {
        *error = ec && ec != std::errc::no_such_file_or_directory ? FromErrorCode(ec) : CELL_ENOENT;
        return st;
    }
    const bool dir = fs::is_directory(s);
    st.mode = (dir ? CELL_FS_S_IFDIR : CELL_FS_S_IFREG) | ((mount && mount->read_only) ? 0555 : 0777);
    if (!dir) st.mode &= ~0111;  // regular files: rw-rw-rw- (r--r--r-- on read-only devices)
    const auto t = fs::last_write_time(host, ec);
    st.atime = st.mtime = st.ctime = ec ? 0 : UnixTime(t);
    st.size = dir ? 0 : fs::file_size(host, ec);
    if (ec) st.size = 0;
    st.blksize = BlockSize(mount);
    *error = CELL_OK;
    return st;
}

// ---------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------

s32 FsService::Open(std::string_view path, s32 flags, u32* fd) {
    opens_.fetch_add(1, std::memory_order_relaxed);
    const s32 access = flags & CELL_FS_O_ACCMODE;
    if (access == CELL_FS_O_ACCMODE || (flags & ~(CELL_FS_O_ACCMODE | CELL_FS_O_CREAT | CELL_FS_O_EXCL | CELL_FS_O_TRUNC |
                                                  CELL_FS_O_APPEND | CELL_FS_O_MSELF))) {
        LOG_WARN("fs", "open \"%.*s\": invalid flags 0%o", static_cast<int>(path.size()), path.data(), flags);
        return CELL_EINVAL;
    }
    VfsResolved r;
    if (const s32 e = Resolve(path, &r); e != CELL_OK) {
        open_failures_.fetch_add(1, std::memory_order_relaxed);
        NoteMissing(path);
        LOG_WARN("fs", "open \"%.*s\": %s", static_cast<int>(path.size()), path.data(),
                 e == CELL_ENOTMOUNTED ? "device not mounted (see --dev-bdvd / --dev-hdd0 / --app-home)" : "invalid path");
        return e;
    }
    const bool writes = access != CELL_FS_O_RDONLY || (flags & (CELL_FS_O_CREAT | CELL_FS_O_TRUNC | CELL_FS_O_APPEND));
    if (writes && r.mount->read_only) {
        LOG_WARN("fs", "open \"%.*s\" (%s, 0%o): %s is read-only", static_cast<int>(path.size()), path.data(), AccessName(flags),
                 flags, r.mount->prefix.c_str());
        return CELL_EROFS;
    }
    std::error_code ec;
    if (fs::is_directory(r.host, ec)) return CELL_EISDIR;

    u32 hflags = 0;
    if (access != CELL_FS_O_WRONLY) hflags |= host::kFileRead;
    if (access != CELL_FS_O_RDONLY) hflags |= host::kFileWrite;
    if (flags & CELL_FS_O_CREAT) hflags |= host::kFileCreate | host::kFileWrite;
    if (flags & CELL_FS_O_EXCL) hflags |= host::kFileExclusive;
    if (flags & CELL_FS_O_TRUNC) hflags |= host::kFileTruncate | host::kFileWrite;
    host::FileError err = host::FileError::None;
    auto file = host::File::Open(r.host, hflags, &err);
    if (!file) {
        open_failures_.fetch_add(1, std::memory_order_relaxed);
        const s32 e = FromFileError(err);
        if (e == CELL_ENOENT) {
            NoteMissing(path);
            LOG_WARN("fs", "open \"%.*s\": not found (host %s)", static_cast<int>(path.size()), path.data(), r.host.string().c_str());
        } else {
            LOG_WARN("fs", "open \"%.*s\": error 0x%08x (host %s)", static_cast<int>(path.size()), path.data(), static_cast<u32>(e),
                     r.host.string().c_str());
        }
        return e;
    }
    auto h = std::make_shared<FsHandle>();
    h->kind = FsHandle::Kind::File;
    h->path = std::string(path);
    h->host = r.host;
    h->mount = r.mount;
    h->flags = flags;
    h->file = std::move(*file);
    const u64 size = h->file.Size().value_or(0);
    if (const s32 e = Insert(h, fd); e != CELL_OK) {
        LOG_WARN("fs", "open \"%.*s\": out of file descriptors", static_cast<int>(path.size()), path.data());
        return e;
    }
    LOG_INFO("fs", "open \"%s\" (%s%s%s) -> fd %u, %" PRIu64 " bytes", h->path.c_str(), AccessName(flags),
             (flags & CELL_FS_O_CREAT) ? ",creat" : "", (flags & CELL_FS_O_TRUNC) ? ",trunc" : "", *fd, size);
    return CELL_OK;
}

s32 FsService::Close(u32 fd) {
    std::shared_ptr<FsHandle> h;
    {
        std::lock_guard lock(mutex_);
        if (fd < kFsFirstFd || fd >= kFsFdLimit || !fds_[fd] || fds_[fd]->kind != FsHandle::Kind::File) return CELL_EBADF;
        h = std::move(fds_[fd]);
    }
    LOG_TRACE("fs", "close fd %u \"%s\" (%" PRIu64 " bytes read, %" PRIu64 " written)", fd, h->path.c_str(), h->bytes_read.load(),
              h->bytes_written.load());
    return CELL_OK;  // the host handle closes when the last in-flight call drops its reference
}

s32 FsService::ReadAt(u32 fd, u64 offset, u32 buffer, u64 size, u64* done) {
    *done = 0;
    const auto h = Get(fd, FsHandle::Kind::File);
    if (!h) return CELL_EBADF;
    return DoReadAt(*h, offset, buffer, size, done);
}

s32 FsService::DoReadAt(FsHandle& handle, u64 offset, u32 buffer, u64 size, u64* done) {
    *done = 0;
    FsHandle* const h = &handle;
    if ((h->flags & CELL_FS_O_ACCMODE) == CELL_FS_O_WRONLY) return CELL_EBADF;
    const u64 file_size = h->file.Size().value_or(0);
    const u64 avail = offset < file_size ? std::min(size, file_size - offset) : 0;
    if (!avail) return CELL_OK;
    vm::GuestMemory& m = kernel_.memory();
    if (avail > 0x100000000ull - buffer || !m.Check(buffer, avail, vm::kPageWritable)) return CELL_EFAULT;
    const host::FileError e = h->file.ReadAt(offset, m.shadow() + buffer, avail, done);
    InvalidateCode(buffer, *done);
    h->bytes_read.fetch_add(*done, std::memory_order_relaxed);
    bytes_read_.fetch_add(*done, std::memory_order_relaxed);
    reads_.fetch_add(1, std::memory_order_relaxed);
    return FromFileError(e);
}

s32 FsService::WriteAt(u32 fd, u64 offset, u32 buffer, u64 size, u64* done) {
    *done = 0;
    const auto h = Get(fd, FsHandle::Kind::File);
    if (!h) return CELL_EBADF;
    return DoWriteAt(*h, offset, buffer, size, done);
}

s32 FsService::DoWriteAt(FsHandle& handle, u64 offset, u32 buffer, u64 size, u64* done) {
    *done = 0;
    FsHandle* const h = &handle;
    if ((h->flags & CELL_FS_O_ACCMODE) == CELL_FS_O_RDONLY) return CELL_EBADF;
    if (!size) return CELL_OK;
    vm::GuestMemory& m = kernel_.memory();
    if (size > 0x100000000ull - buffer || !m.Check(buffer, size, vm::kPageReadable)) return CELL_EFAULT;
    const host::FileError e = h->file.WriteAt(offset, m.shadow() + buffer, size, done);
    h->bytes_written.fetch_add(*done, std::memory_order_relaxed);
    bytes_written_.fetch_add(*done, std::memory_order_relaxed);
    return FromFileError(e);
}

s32 FsService::Read(u32 fd, u32 buffer, u64 size, u64* done) {
    *done = 0;
    const auto h = Get(fd, FsHandle::Kind::File);
    if (!h) return CELL_EBADF;
    std::lock_guard lock(h->m);  // read + advance is one step for this fd
    const s32 r = DoReadAt(*h, h->pos, buffer, size, done);
    h->pos += *done;
    return r;
}

s32 FsService::Write(u32 fd, u32 buffer, u64 size, u64* done) {
    *done = 0;
    const auto h = Get(fd, FsHandle::Kind::File);
    if (!h) return CELL_EBADF;
    std::lock_guard lock(h->m);
    if (h->flags & CELL_FS_O_APPEND) h->pos = h->file.Size().value_or(h->pos);
    const s32 r = DoWriteAt(*h, h->pos, buffer, size, done);
    h->pos += *done;
    return r;
}

s32 FsService::Seek(u32 fd, s64 offset, s32 whence, u64* pos) {
    const auto h = Get(fd, FsHandle::Kind::File);
    if (!h) return CELL_EBADF;
    std::lock_guard lock(h->m);
    s64 base = 0;
    switch (whence) {
    case CELL_FS_SEEK_SET: base = 0; break;
    case CELL_FS_SEEK_CUR: base = static_cast<s64>(h->pos); break;
    case CELL_FS_SEEK_END: base = static_cast<s64>(h->file.Size().value_or(0)); break;
    default: return CELL_EINVAL;
    }
    const s64 target = base + offset;
    if (target < 0) return CELL_EINVAL;
    h->pos = static_cast<u64>(target);
    *pos = h->pos;
    return CELL_OK;
}

s32 FsService::Fstat(u32 fd, FsStat* out) {
    const auto h = Get(fd, FsHandle::Kind::File);
    if (!h) return CELL_EBADF;
    s32 e = CELL_OK;
    *out = StatOf(h->host, h->mount, &e);
    if (e != CELL_OK) {  // file removed while open: still answer for the handle
        *out = FsStat{};
        out->mode = CELL_FS_S_IFREG | 0666;
        out->blksize = BlockSize(h->mount);
    }
    out->size = h->file.Size().value_or(out->size);
    return CELL_OK;
}

s32 FsService::Ftruncate(u32 fd, u64 size) {
    const auto h = Get(fd, FsHandle::Kind::File);
    if (!h) return CELL_EBADF;
    if ((h->flags & CELL_FS_O_ACCMODE) == CELL_FS_O_RDONLY) return CELL_EBADF;
    return FromFileError(h->file.Truncate(size));
}

s32 FsService::Fsync(u32 fd) {
    const auto h = Get(fd, FsHandle::Kind::File);
    if (!h) return CELL_EBADF;
    if ((h->flags & CELL_FS_O_ACCMODE) == CELL_FS_O_RDONLY) return CELL_OK;
    return FromFileError(h->file.Sync());
}

s32 FsService::FGetBlockSize(u32 fd, u64* sector_size, u64* block_size) {
    const auto h = Get(fd, FsHandle::Kind::File);
    if (!h) return CELL_EBADF;
    *sector_size = SectorSize(h->mount);
    *block_size = BlockSize(h->mount);
    return CELL_OK;
}

// ---------------------------------------------------------------------------------------
// Paths and directories
// ---------------------------------------------------------------------------------------

s32 FsService::Stat(std::string_view path, FsStat* out) {
    if (path == "/") {  // the device root: a directory of mount points
        *out = FsStat{};
        out->mode = CELL_FS_S_IFDIR | 0555;
        out->blksize = 4096;
        return CELL_OK;
    }
    VfsResolved r;
    if (const s32 e = Resolve(path, &r); e != CELL_OK) return e;
    s32 e = CELL_OK;
    *out = StatOf(r.host, r.mount, &e);
    if (e == CELL_ENOENT) LOG_TRACE("fs", "stat \"%.*s\": not found", static_cast<int>(path.size()), path.data());
    return e;
}

s32 FsService::OpenDir(std::string_view path, u32* fd) {
    auto h = std::make_shared<FsHandle>();
    h->kind = FsHandle::Kind::Dir;
    h->path = std::string(path);
    h->entries.push_back({CELL_FS_TYPE_DIRECTORY, "."});
    h->entries.push_back({CELL_FS_TYPE_DIRECTORY, ".."});
    std::vector<FsDirent> list;
    if (path == "/") {
        for (const VfsMount& m : kernel_.vfs().Mounts())
            if (m.prefix.size() > 1 && m.prefix.find('/', 1) == std::string::npos) list.push_back({CELL_FS_TYPE_DIRECTORY, m.prefix.substr(1)});
    } else {
        VfsResolved r;
        if (const s32 e = Resolve(path, &r); e != CELL_OK) return e;
        std::error_code ec;
        if (!fs::exists(r.host, ec)) {
            NoteMissing(path);
            return CELL_ENOENT;
        }
        if (!fs::is_directory(r.host, ec)) return CELL_ENOTDIR;
        for (fs::directory_iterator it(r.host, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            const u8 type = it->is_symlink(e2)     ? CELL_FS_TYPE_SYMLINK
                            : it->is_directory(e2) ? CELL_FS_TYPE_DIRECTORY
                                                   : CELL_FS_TYPE_REGULAR;
            list.push_back({type, it->path().filename().string()});
        }
        if (ec) return FromErrorCode(ec);
        h->host = r.host;
        h->mount = r.mount;
    }
    std::sort(list.begin(), list.end(), [](const FsDirent& a, const FsDirent& b) { return a.name < b.name; });
    h->entries.insert(h->entries.end(), list.begin(), list.end());
    if (const s32 e = Insert(h, fd); e != CELL_OK) return e;
    LOG_TRACE("fs", "opendir \"%s\" -> fd %u, %zu entries", h->path.c_str(), *fd, h->entries.size());
    return CELL_OK;
}

s32 FsService::ReadDir(u32 fd, FsDirent* out, bool* end) {
    const auto h = Get(fd, FsHandle::Kind::Dir);
    if (!h) return CELL_EBADF;
    std::lock_guard lock(h->m);
    *end = h->next >= h->entries.size();
    if (!*end) *out = h->entries[h->next++];
    return CELL_OK;
}

s32 FsService::CloseDir(u32 fd) {
    std::lock_guard lock(mutex_);
    if (fd < kFsFirstFd || fd >= kFsFdLimit || !fds_[fd] || fds_[fd]->kind != FsHandle::Kind::Dir) return CELL_EBADF;
    fds_[fd].reset();
    return CELL_OK;
}

s32 FsService::Mkdir(std::string_view path) {
    VfsResolved r;
    if (const s32 e = Resolve(path, &r); e != CELL_OK) return e;
    if (r.mount->read_only) return CELL_EROFS;
    std::error_code ec;
    if (fs::exists(r.host, ec)) return CELL_EEXIST;
    if (!fs::exists(r.host.parent_path(), ec)) return CELL_ENOENT;
    if (!fs::create_directory(r.host, ec)) return ec ? FromErrorCode(ec) : CELL_EEXIST;
    LOG_INFO("fs", "mkdir \"%.*s\"", static_cast<int>(path.size()), path.data());
    return CELL_OK;
}

s32 FsService::Rmdir(std::string_view path) {
    VfsResolved r;
    if (const s32 e = Resolve(path, &r); e != CELL_OK) return e;
    if (r.mount->read_only) return CELL_EROFS;
    std::error_code ec;
    if (!fs::exists(r.host, ec)) return CELL_ENOENT;
    if (!fs::is_directory(r.host, ec)) return CELL_ENOTDIR;
    if (!fs::is_empty(r.host, ec)) return CELL_ENOTEMPTY;
    fs::remove(r.host, ec);
    return ec ? FromErrorCode(ec) : CELL_OK;
}

s32 FsService::Unlink(std::string_view path) {
    VfsResolved r;
    if (const s32 e = Resolve(path, &r); e != CELL_OK) return e;
    if (r.mount->read_only) return CELL_EROFS;
    std::error_code ec;
    if (!fs::exists(r.host, ec)) return CELL_ENOENT;
    if (fs::is_directory(r.host, ec)) return CELL_EISDIR;
    fs::remove(r.host, ec);
    return ec ? FromErrorCode(ec) : CELL_OK;
}

s32 FsService::Rename(std::string_view from, std::string_view to) {
    VfsResolved a, b;
    if (const s32 e = Resolve(from, &a); e != CELL_OK) return e;
    if (const s32 e = Resolve(to, &b); e != CELL_OK) return e;
    if (a.mount != b.mount) return CELL_EXDEV;
    if (a.mount->read_only) return CELL_EROFS;
    std::error_code ec;
    if (!fs::exists(a.host, ec)) return CELL_ENOENT;
    fs::rename(a.host, b.host, ec);
    return ec ? FromErrorCode(ec) : CELL_OK;
}

s32 FsService::Truncate(std::string_view path, u64 size) {
    VfsResolved r;
    if (const s32 e = Resolve(path, &r); e != CELL_OK) return e;
    if (r.mount->read_only) return CELL_EROFS;
    std::error_code ec;
    if (fs::is_directory(r.host, ec)) return CELL_EISDIR;
    host::FileError err = host::FileError::None;
    auto f = host::File::Open(r.host, host::kFileWrite, &err);
    if (!f) return FromFileError(err);
    return FromFileError(f->Truncate(size));
}

s32 FsService::GetBlockSize(std::string_view path, u64* sector_size, u64* block_size) {
    VfsResolved r;
    if (const s32 e = Resolve(path, &r); e != CELL_OK) return e;
    std::error_code ec;
    if (!fs::exists(r.host, ec)) return CELL_ENOENT;
    *sector_size = SectorSize(r.mount);
    *block_size = BlockSize(r.mount);
    return CELL_OK;
}

s32 FsService::GetFreeSize(std::string_view path, u64* block_size, u64* block_count) {
    VfsResolved r;
    if (const s32 e = Resolve(path, &r); e != CELL_OK) return e;
    *block_size = BlockSize(r.mount);
    if (r.mount->read_only) {
        *block_count = 0;
        return CELL_OK;
    }
    std::error_code ec;
    const fs::space_info s = fs::space(r.mount->host, ec);
    *block_count = ec ? 0 : s.available / *block_size;
    return CELL_OK;
}

// ---------------------------------------------------------------------------------------
// Guest ABI
// ---------------------------------------------------------------------------------------

namespace {
bool OutOk(const vm::GuestMemory& m, u32 ea, u32 size) { return !ea || m.Check(ea, size, vm::kPageWritable); }
}  // namespace

s32 FsService::GuestOpen(u32 path, s32 flags, u32 fd_out) {
    vm::GuestMemory& m = kernel_.memory();
    std::string p;
    if (const s32 e = ReadPath(path, &p); e != CELL_OK) return e;
    if (!fd_out || !m.Check(fd_out, 4, vm::kPageWritable)) return CELL_EFAULT;
    u32 fd = 0;
    const s32 e = Open(p, flags, &fd);
    if (e == CELL_OK) m.Write<u32>(fd_out, fd);
    return e;
}

s32 FsService::GuestRead(u32 fd, u32 buffer, u64 size, u32 done_out) {
    vm::GuestMemory& m = kernel_.memory();
    if (!OutOk(m, done_out, 8)) return CELL_EFAULT;
    u64 done = 0;
    const s32 e = Read(fd, buffer, size, &done);
    if (done_out) m.Write<u64>(done_out, done);
    return e;
}

s32 FsService::GuestWrite(u32 fd, u32 buffer, u64 size, u32 done_out) {
    vm::GuestMemory& m = kernel_.memory();
    if (!OutOk(m, done_out, 8)) return CELL_EFAULT;
    u64 done = 0;
    const s32 e = Write(fd, buffer, size, &done);
    if (done_out) m.Write<u64>(done_out, done);
    return e;
}

s32 FsService::GuestReadAt(u32 fd, u64 offset, u32 buffer, u64 size, u32 done_out) {
    vm::GuestMemory& m = kernel_.memory();
    if (!OutOk(m, done_out, 8)) return CELL_EFAULT;
    u64 done = 0;
    const s32 e = ReadAt(fd, offset, buffer, size, &done);
    if (done_out) m.Write<u64>(done_out, done);
    return e;
}

s32 FsService::GuestWriteAt(u32 fd, u64 offset, u32 buffer, u64 size, u32 done_out) {
    vm::GuestMemory& m = kernel_.memory();
    if (!OutOk(m, done_out, 8)) return CELL_EFAULT;
    u64 done = 0;
    const s32 e = WriteAt(fd, offset, buffer, size, &done);
    if (done_out) m.Write<u64>(done_out, done);
    return e;
}

s32 FsService::GuestSeek(u32 fd, s64 offset, s32 whence, u32 pos_out) {
    vm::GuestMemory& m = kernel_.memory();
    if (!OutOk(m, pos_out, 8)) return CELL_EFAULT;
    u64 pos = 0;
    const s32 e = Seek(fd, offset, whence, &pos);
    if (e == CELL_OK && pos_out) m.Write<u64>(pos_out, pos);
    return e;
}

s32 FsService::GuestStat(u32 path, u32 stat_out) {
    vm::GuestMemory& m = kernel_.memory();
    std::string p;
    if (const s32 e = ReadPath(path, &p); e != CELL_OK) return e;
    if (!stat_out || !m.Check(stat_out, kFsStatSize, vm::kPageWritable)) return CELL_EFAULT;
    FsStat st;
    const s32 e = Stat(p, &st);
    if (e == CELL_OK) WriteFsStat(m, stat_out, st);
    return e;
}

s32 FsService::GuestFstat(u32 fd, u32 stat_out) {
    vm::GuestMemory& m = kernel_.memory();
    if (!stat_out || !m.Check(stat_out, kFsStatSize, vm::kPageWritable)) return CELL_EFAULT;
    FsStat st;
    const s32 e = Fstat(fd, &st);
    if (e == CELL_OK) WriteFsStat(m, stat_out, st);
    return e;
}

s32 FsService::GuestOpenDir(u32 path, u32 fd_out) {
    vm::GuestMemory& m = kernel_.memory();
    std::string p;
    if (const s32 e = ReadPath(path, &p); e != CELL_OK) return e;
    if (!fd_out || !m.Check(fd_out, 4, vm::kPageWritable)) return CELL_EFAULT;
    u32 fd = 0;
    const s32 e = OpenDir(p, &fd);
    if (e == CELL_OK) m.Write<u32>(fd_out, fd);
    return e;
}

// nread = sizeof(CellFsDirent) per entry, 0 at the end of the directory.
s32 FsService::GuestReadDir(u32 fd, u32 dirent_out, u32 nread_out) {
    vm::GuestMemory& m = kernel_.memory();
    if (!dirent_out || !m.Check(dirent_out, kFsDirentSize, vm::kPageWritable) || !nread_out ||
        !m.Check(nread_out, 8, vm::kPageWritable))
        return CELL_EFAULT;
    FsDirent d;
    bool end = false;
    const s32 e = ReadDir(fd, &d, &end);
    if (e != CELL_OK) return e;
    if (!end) WriteFsDirent(m, dirent_out, d);
    m.Write<u64>(nread_out, end ? 0 : kFsDirentSize);
    return CELL_OK;
}

s32 FsService::GuestPathOp(u32 path, s32 (FsService::*op)(std::string_view)) {
    std::string p;
    if (const s32 e = ReadPath(path, &p); e != CELL_OK) return e;
    return (this->*op)(p);
}

s32 FsService::GuestRename(u32 from, u32 to) {
    std::string a, b;
    if (const s32 e = ReadPath(from, &a); e != CELL_OK) return e;
    if (const s32 e = ReadPath(to, &b); e != CELL_OK) return e;
    return Rename(a, b);
}

s32 FsService::GuestTruncate(u32 path, u64 size) {
    std::string p;
    if (const s32 e = ReadPath(path, &p); e != CELL_OK) return e;
    return Truncate(p, size);
}

s32 FsService::GuestGetBlockSize(u32 path, u32 sector_out, u32 block_out) {
    vm::GuestMemory& m = kernel_.memory();
    std::string p;
    if (const s32 e = ReadPath(path, &p); e != CELL_OK) return e;
    if (!sector_out || !block_out || !m.Check(sector_out, 8, vm::kPageWritable) || !m.Check(block_out, 8, vm::kPageWritable))
        return CELL_EFAULT;
    u64 sector = 0, block = 0;
    const s32 e = GetBlockSize(p, &sector, &block);
    if (e == CELL_OK) {
        m.Write<u64>(sector_out, sector);
        m.Write<u64>(block_out, block);
    }
    return e;
}

s32 FsService::GuestFGetBlockSize(u32 fd, u32 sector_out, u32 block_out) {
    vm::GuestMemory& m = kernel_.memory();
    if (!sector_out || !block_out || !m.Check(sector_out, 8, vm::kPageWritable) || !m.Check(block_out, 8, vm::kPageWritable))
        return CELL_EFAULT;
    u64 sector = 0, block = 0;
    const s32 e = FGetBlockSize(fd, &sector, &block);
    if (e == CELL_OK) {
        m.Write<u64>(sector_out, sector);
        m.Write<u64>(block_out, block);
    }
    return e;
}

s32 FsService::GuestGetFreeSize(u32 path, u32 block_size_out, u32 block_count_out) {
    vm::GuestMemory& m = kernel_.memory();
    std::string p;
    if (const s32 e = ReadPath(path, &p); e != CELL_OK) return e;
    if (!block_size_out || !block_count_out || !m.Check(block_size_out, 4, vm::kPageWritable) ||
        !m.Check(block_count_out, 8, vm::kPageWritable))
        return CELL_EFAULT;
    u64 bs = 0, count = 0;
    const s32 e = GetFreeSize(p, &bs, &count);
    if (e == CELL_OK) {
        m.Write<u32>(block_size_out, static_cast<u32>(bs));
        m.Write<u64>(block_count_out, count);
    }
    return e;
}

// ---------------------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------------------

std::vector<std::string> FsService::Describe() const {
    std::vector<std::string> out;
    char line[1280];
    std::snprintf(line, sizeof(line), "%" PRIu64 " open(s) (%" PRIu64 " failed), %" PRIu64 " read call(s), %" PRIu64
                  " bytes read, %" PRIu64 " bytes written",
                  opens_.load(), open_failures_.load(), reads_.load(), bytes_read_.load(), bytes_written_.load());
    out.emplace_back(line);
    std::vector<std::pair<u32, std::shared_ptr<FsHandle>>> open;
    std::deque<std::string> missing;
    {
        std::lock_guard lock(mutex_);
        for (u32 i = kFsFirstFd; i < kFsFdLimit; ++i)
            if (fds_[i]) open.emplace_back(i, fds_[i]);
        missing = missing_;
    }
    for (const auto& [fd, h] : open) {
        if (h->kind == FsHandle::Kind::Dir) {
            std::snprintf(line, sizeof(line), "fd %u dir  \"%s\"", fd, h->path.c_str());
        } else {
            u64 pos = 0;
            {
                std::unique_lock lock(h->m, std::try_to_lock);  // never block the report
                pos = lock ? h->pos : ~u64{0};
            }
            std::snprintf(line, sizeof(line), "fd %u file \"%s\" (%s) size 0x%" PRIx64 ", pos %s0x%" PRIx64 ", read 0x%" PRIx64
                          ", written 0x%" PRIx64,
                          fd, h->path.c_str(), AccessName(h->flags), h->file.Size().value_or(0), pos == ~u64{0} ? "(busy) " : "",
                          pos == ~u64{0} ? 0 : pos, h->bytes_read.load(), h->bytes_written.load());
        }
        out.emplace_back(line);
    }
    for (const std::string& p : missing) out.push_back("missing: \"" + p + "\"");
    return out;
}

// ---------------------------------------------------------------------------------------
// Syscalls 801..834 (lv2 numbering, as RPCS3)
// ---------------------------------------------------------------------------------------

void FsService::RegisterSyscalls(SyscallTable& table) {
    using C = ppu::PpuThreadContext;
    static constexpr auto S = [](void* p) -> FsService& { return *static_cast<FsService*>(p); };
    static constexpr auto A32 = [](const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); };

    // sys_fs_open(path, flags, fd*, mode, arg, size)
    table.Register(801, "sys_fs_open", [](void* p, C& c) -> s32 { return S(p).GuestOpen(A32(c, 0), static_cast<s32>(c.gpr[4]), A32(c, 2)); }, this);
    table.Register(802, "sys_fs_read", [](void* p, C& c) -> s32 { return S(p).GuestRead(A32(c, 0), A32(c, 1), c.gpr[5], A32(c, 3)); }, this);
    table.Register(803, "sys_fs_write", [](void* p, C& c) -> s32 { return S(p).GuestWrite(A32(c, 0), A32(c, 1), c.gpr[5], A32(c, 3)); }, this);
    table.Register(804, "sys_fs_close", [](void* p, C& c) -> s32 { return S(p).Close(A32(c, 0)); }, this);
    table.Register(805, "sys_fs_opendir", [](void* p, C& c) -> s32 { return S(p).GuestOpenDir(A32(c, 0), A32(c, 1)); }, this);
    table.Register(806, "sys_fs_readdir", [](void* p, C& c) -> s32 { return S(p).GuestReadDir(A32(c, 0), A32(c, 1), A32(c, 2)); }, this);
    table.Register(807, "sys_fs_closedir", [](void* p, C& c) -> s32 { return S(p).CloseDir(A32(c, 0)); }, this);
    table.Register(808, "sys_fs_stat", [](void* p, C& c) -> s32 { return S(p).GuestStat(A32(c, 0), A32(c, 1)); }, this);
    table.Register(809, "sys_fs_fstat", [](void* p, C& c) -> s32 { return S(p).GuestFstat(A32(c, 0), A32(c, 1)); }, this);
    table.Register(811, "sys_fs_mkdir", [](void* p, C& c) -> s32 { return S(p).GuestPathOp(A32(c, 0), &FsService::Mkdir); }, this);
    table.Register(812, "sys_fs_rename", [](void* p, C& c) -> s32 { return S(p).GuestRename(A32(c, 0), A32(c, 1)); }, this);
    table.Register(813, "sys_fs_rmdir", [](void* p, C& c) -> s32 { return S(p).GuestPathOp(A32(c, 0), &FsService::Rmdir); }, this);
    table.Register(814, "sys_fs_unlink", [](void* p, C& c) -> s32 { return S(p).GuestPathOp(A32(c, 0), &FsService::Unlink); }, this);
    table.Register(818, "sys_fs_lseek", [](void* p, C& c) -> s32 {
        return S(p).GuestSeek(A32(c, 0), static_cast<s64>(c.gpr[4]), static_cast<s32>(c.gpr[5]), A32(c, 3));
    }, this);
    table.Register(820, "sys_fs_fsync", [](void* p, C& c) -> s32 { return S(p).Fsync(A32(c, 0)); }, this);
    table.Register(821, "sys_fs_fget_block_size", [](void* p, C& c) -> s32 { return S(p).GuestFGetBlockSize(A32(c, 0), A32(c, 1), A32(c, 2)); }, this);
    table.Register(822, "sys_fs_get_block_size", [](void* p, C& c) -> s32 { return S(p).GuestGetBlockSize(A32(c, 0), A32(c, 1), A32(c, 2)); }, this);
    table.Register(831, "sys_fs_truncate", [](void* p, C& c) -> s32 { return S(p).GuestTruncate(A32(c, 0), c.gpr[4]); }, this);
    table.Register(832, "sys_fs_ftruncate", [](void* p, C& c) -> s32 { return S(p).Ftruncate(A32(c, 0), c.gpr[4]); }, this);
    // sys_fs_chmod(path, mode): host permissions are not mapped; succeed if the path exists.
    table.Register(834, "sys_fs_chmod", [](void* p, C& c) -> s32 {
        FsService& s = S(p);
        std::string path;
        if (const s32 e = s.ReadPath(A32(c, 0), &path); e != CELL_OK) return e;
        FsStat st;
        return s.Stat(path, &st);
    }, this);
}

}  // namespace cell::lv2
