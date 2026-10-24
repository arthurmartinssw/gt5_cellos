// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — common/host_file.cpp
#include "common/host_file.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cell::host {

File::~File() { Close(); }

File::File(File&& other) noexcept {
#ifdef _WIN32
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        Close();
#ifdef _WIN32
        handle_ = other.handle_;
        other.handle_ = nullptr;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
    }
    return *this;
}

#ifdef _WIN32
// =======================================================================================
// Windows: CreateFileW + ReadFile/WriteFile with an OVERLAPPED offset (synchronous handle).
// =======================================================================================
namespace {
FileError FromWin32(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME: return FileError::NotFound;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return FileError::Exists;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_WRITE_PROTECT: return FileError::Access;
    case ERROR_DIRECTORY: return FileError::NotDir;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return FileError::NoSpace;
    case ERROR_TOO_MANY_OPEN_FILES: return FileError::TooMany;
    case ERROR_FILENAME_EXCED_RANGE: return FileError::NameTooLong;
    default: return FileError::Io;
    }
}
constexpr DWORD kChunk = 1u << 30;  // ReadFile/WriteFile take a DWORD count
}  // namespace

std::optional<File> File::Open(const std::filesystem::path& path, u32 flags, FileError* error) {
    DWORD access = 0;
    if (flags & kFileRead) access |= GENERIC_READ;
    if (flags & kFileWrite) access |= GENERIC_WRITE;
    DWORD disposition = OPEN_EXISTING;
    if ((flags & kFileCreate) && (flags & kFileExclusive)) disposition = CREATE_NEW;
    else if ((flags & kFileCreate) && (flags & kFileTruncate)) disposition = CREATE_ALWAYS;
    else if (flags & kFileCreate) disposition = OPEN_ALWAYS;
    else if (flags & kFileTruncate) disposition = TRUNCATE_EXISTING;
    HANDLE h = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *error = FromWin32(GetLastError());
        return std::nullopt;
    }
    File f;
    f.handle_ = h;
    *error = FileError::None;
    return f;
}

bool File::is_open() const noexcept { return handle_ != nullptr; }

FileError File::ReadAt(u64 offset, void* buffer, u64 size, u64* done) const {
    *done = 0;
    auto* out = static_cast<u8*>(buffer);
    while (*done < size) {
        const DWORD want = static_cast<DWORD>(std::min<u64>(size - *done, kChunk));
        const u64 at = offset + *done;
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(at);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), out + *done, want, &got, &ov)) {
            const DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) return FileError::None;
            return FromWin32(e);
        }
        if (got == 0) break;  // end of file
        *done += got;
    }
    return FileError::None;
}

FileError File::WriteAt(u64 offset, const void* buffer, u64 size, u64* done) const {
    *done = 0;
    const auto* in = static_cast<const u8*>(buffer);
    while (*done < size) {
        const DWORD want = static_cast<DWORD>(std::min<u64>(size - *done, kChunk));
        const u64 at = offset + *done;
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(at);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD put = 0;
        if (!WriteFile(static_cast<HANDLE>(handle_), in + *done, want, &put, &ov)) return FromWin32(GetLastError());
        if (put == 0) return FileError::Io;
        *done += put;
    }
    return FileError::None;
}

std::optional<u64> File::Size() const {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(static_cast<HANDLE>(handle_), &size)) return std::nullopt;
    return static_cast<u64>(size.QuadPart);
}

FileError File::Truncate(u64 size) const {
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFileInformationByHandle(static_cast<HANDLE>(handle_), FileEndOfFileInfo, &info, sizeof(info)))
        return FromWin32(GetLastError());
    return FileError::None;
}

FileError File::Sync() const {
    return FlushFileBuffers(static_cast<HANDLE>(handle_)) ? FileError::None : FromWin32(GetLastError());
}

void File::Close() noexcept {
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
}

#else
// =======================================================================================
// POSIX (Linux, macOS): open + pread/pwrite (64-bit off_t on every 64-bit host).
// =======================================================================================
namespace {
FileError FromErrno(int e) {
    switch (e) {
    case ENOENT: return FileError::NotFound;
    case EEXIST: return FileError::Exists;
    case EACCES:
    case EPERM:
    case EROFS: return FileError::Access;
    case EISDIR: return FileError::IsDir;
    case ENOTDIR: return FileError::NotDir;
    case ENOSPC: return FileError::NoSpace;
    case EMFILE:
    case ENFILE: return FileError::TooMany;
    case ENAMETOOLONG: return FileError::NameTooLong;
    default: return FileError::Io;
    }
}
constexpr u64 kChunk = 1ull << 30;
}  // namespace

std::optional<File> File::Open(const std::filesystem::path& path, u32 flags, FileError* error) {
    int oflags = O_CLOEXEC;
    if ((flags & kFileRead) && (flags & kFileWrite)) oflags |= O_RDWR;
    else if (flags & kFileWrite) oflags |= O_WRONLY;
    else oflags |= O_RDONLY;
    if (flags & kFileCreate) oflags |= O_CREAT;
    if (flags & kFileExclusive) oflags |= O_EXCL;
    if (flags & kFileTruncate) oflags |= O_TRUNC;
    int fd;
    do fd = ::open(path.c_str(), oflags, 0666);
    while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        *error = FromErrno(errno);
        return std::nullopt;
    }
    struct stat st {};
    if (::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {  // POSIX lets O_RDONLY open a directory
        ::close(fd);
        *error = FileError::IsDir;
        return std::nullopt;
    }
    File f;
    f.fd_ = fd;
    *error = FileError::None;
    return f;
}

bool File::is_open() const noexcept { return fd_ >= 0; }

FileError File::ReadAt(u64 offset, void* buffer, u64 size, u64* done) const {
    *done = 0;
    auto* out = static_cast<u8*>(buffer);
    while (*done < size) {
        const ssize_t got = ::pread(fd_, out + *done, static_cast<std::size_t>(std::min(size - *done, kChunk)),
                                    static_cast<off_t>(offset + *done));
        if (got < 0) {
            if (errno == EINTR) continue;
            return FromErrno(errno);
        }
        if (got == 0) break;  // end of file
        *done += static_cast<u64>(got);
    }
    return FileError::None;
}

FileError File::WriteAt(u64 offset, const void* buffer, u64 size, u64* done) const {
    *done = 0;
    const auto* in = static_cast<const u8*>(buffer);
    while (*done < size) {
        const ssize_t put = ::pwrite(fd_, in + *done, static_cast<std::size_t>(std::min(size - *done, kChunk)),
                                     static_cast<off_t>(offset + *done));
        if (put < 0) {
            if (errno == EINTR) continue;
            return FromErrno(errno);
        }
        if (put == 0) return FileError::Io;
        *done += static_cast<u64>(put);
    }
    return FileError::None;
}

std::optional<u64> File::Size() const {
    struct stat st {};
    if (::fstat(fd_, &st) != 0) return std::nullopt;
    return static_cast<u64>(st.st_size);
}

FileError File::Truncate(u64 size) const {
    return ::ftruncate(fd_, static_cast<off_t>(size)) == 0 ? FileError::None : FromErrno(errno);
}

FileError File::Sync() const { return ::fsync(fd_) == 0 ? FileError::None : FromErrno(errno); }

void File::Close() noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}
#endif

}  // namespace cell::host
