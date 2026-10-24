// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_fs.h
//
// lv2 file system: guest file descriptors over host files, through the VFS mounts.
//
// On the console, games call cellFs* (libfs, sysPrxForUser's sibling "sys_fs" library),
// which wraps the lv2 syscalls 801..834. Here both layers exist over one service:
//   * hle/cell_fs.cpp registers the cellFs* imports (what a game's EBOOT binds to);
//   * RegisterSyscalls() installs sys_fs_* (what an LLE PRX would call directly).
// Both go through the "guest ABI" methods below (guest pointers in, CELL_* out), so their
// semantics cannot drift apart.
//
// Model:
//   * fds 3..255 like lv2 (0..2 are the TTY on the console); EMFILE past that.
//   * one host handle per guest fd; the file position lives in the fd (mutex-guarded), all
//     host I/O is positional (host::File::ReadAt/WriteAt). cellFsReadWithOffset therefore
//     never touches the position and runs concurrently with other readers of the same fd —
//     the access pattern of big archives (GT5's GT.VOL: table of contents, then random
//     reads of compressed chunks from several threads).
//   * reads go straight from the host file into the guest arena (no bounce buffer); pages
//     holding translated code are invalidated afterwards.
//   * 64-bit sizes and offsets end to end (archives larger than 4 GiB).
//   * /dev_bdvd and /dev_flash are read-only: write/create/truncate/unlink -> CELL_EROFS.
//   * every open is logged; missing paths are kept (last 16) for the end/stall reports, which
//     is usually the first clue when a game stops at boot.
// Not implemented yet (they stay logging stubs at the cellFs level): AIO (cellFsAio*),
// streaming (cellFsSt*), SDATA (cellFsSdata*), sys_fs_fcntl.
#pragma once

#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/host_file.h"
#include "lv2/kernel.h"

namespace cell::lv2 {

// cellFs open flags (octal, as in the SDK).
inline constexpr s32 CELL_FS_O_RDONLY = 0;
inline constexpr s32 CELL_FS_O_WRONLY = 1;
inline constexpr s32 CELL_FS_O_RDWR = 2;
inline constexpr s32 CELL_FS_O_ACCMODE = 3;
inline constexpr s32 CELL_FS_O_CREAT = 0100;
inline constexpr s32 CELL_FS_O_EXCL = 0200;
inline constexpr s32 CELL_FS_O_TRUNC = 01000;
inline constexpr s32 CELL_FS_O_APPEND = 02000;
inline constexpr s32 CELL_FS_O_MSELF = 010000;

inline constexpr s32 CELL_FS_SEEK_SET = 0;
inline constexpr s32 CELL_FS_SEEK_CUR = 1;
inline constexpr s32 CELL_FS_SEEK_END = 2;

inline constexpr s32 CELL_FS_S_IFDIR = 0040000;
inline constexpr s32 CELL_FS_S_IFREG = 0100000;
inline constexpr s32 CELL_FS_S_IFLNK = 0120000;

inline constexpr u8 CELL_FS_TYPE_DIRECTORY = 1;
inline constexpr u8 CELL_FS_TYPE_REGULAR = 2;
inline constexpr u8 CELL_FS_TYPE_SYMLINK = 3;

inline constexpr u32 kFsMaxPath = 1024;    // CELL_FS_MAX_FS_PATH_LENGTH
inline constexpr u32 kFsMaxName = 255;     // CELL_FS_MAX_FS_FILE_NAME_LENGTH
inline constexpr u32 kFsFirstFd = 3;
inline constexpr u32 kFsFdLimit = 256;     // fds 3..255
inline constexpr u32 kFsStatSize = 52;     // CellFsStat (4-byte aligned 64-bit fields)
inline constexpr u32 kFsDirentSize = 258;  // CellFsDirent: u8 type, u8 namlen, char name[256]

struct FsStat {
    s32 mode = 0;
    s32 uid = 0;
    s32 gid = 0;
    s64 atime = 0;
    s64 mtime = 0;
    s64 ctime = 0;
    u64 size = 0;
    u64 blksize = 0;
};

struct FsDirent {
    u8 type = CELL_FS_TYPE_REGULAR;
    std::string name;
};

struct FsHandle {
    enum class Kind : u8 { File, Dir };
    Kind kind = Kind::File;
    std::string path;              // guest path as opened
    std::filesystem::path host;    // resolved host path
    const VfsMount* mount = nullptr;
    // ---- files ----
    host::File file;
    s32 flags = 0;
    std::mutex m;   // guards pos and the read/write sequence of the sequential calls
    u64 pos = 0;
    std::atomic<u64> bytes_read{0};
    std::atomic<u64> bytes_written{0};
    // ---- directories (guarded by m) ----
    std::vector<FsDirent> entries;
    std::size_t next = 0;
};

class FsService {
public:
    explicit FsService(Kernel& kernel) noexcept : kernel_(kernel) {}
    ~FsService();
    FsService(const FsService&) = delete;
    FsService& operator=(const FsService&) = delete;

    // ---- host-level operations (guest paths as strings, guest buffers as EAs) ----
    s32 Open(std::string_view path, s32 flags, u32* fd);
    s32 Close(u32 fd);
    s32 Read(u32 fd, u32 buffer, u64 size, u64* done);
    s32 Write(u32 fd, u32 buffer, u64 size, u64* done);
    s32 ReadAt(u32 fd, u64 offset, u32 buffer, u64 size, u64* done);
    s32 WriteAt(u32 fd, u64 offset, u32 buffer, u64 size, u64* done);
    s32 Seek(u32 fd, s64 offset, s32 whence, u64* pos);
    s32 Stat(std::string_view path, FsStat* out);
    s32 Fstat(u32 fd, FsStat* out);
    s32 OpenDir(std::string_view path, u32* fd);
    s32 ReadDir(u32 fd, FsDirent* out, bool* end);
    s32 CloseDir(u32 fd);
    s32 Mkdir(std::string_view path);
    s32 Rmdir(std::string_view path);
    s32 Unlink(std::string_view path);
    s32 Rename(std::string_view from, std::string_view to);
    s32 Truncate(std::string_view path, u64 size);
    s32 Ftruncate(u32 fd, u64 size);
    s32 Fsync(u32 fd);
    s32 GetBlockSize(std::string_view path, u64* sector_size, u64* block_size);
    s32 FGetBlockSize(u32 fd, u64* sector_size, u64* block_size);
    s32 GetFreeSize(std::string_view path, u64* block_size, u64* block_count);

    // ---- guest ABI (pointers are guest EAs; optional out pointers may be 0) ----
    s32 GuestOpen(u32 path, s32 flags, u32 fd_out);
    s32 GuestRead(u32 fd, u32 buffer, u64 size, u32 done_out);
    s32 GuestWrite(u32 fd, u32 buffer, u64 size, u32 done_out);
    s32 GuestReadAt(u32 fd, u64 offset, u32 buffer, u64 size, u32 done_out);
    s32 GuestWriteAt(u32 fd, u64 offset, u32 buffer, u64 size, u32 done_out);
    s32 GuestSeek(u32 fd, s64 offset, s32 whence, u32 pos_out);
    s32 GuestStat(u32 path, u32 stat_out);
    s32 GuestFstat(u32 fd, u32 stat_out);
    s32 GuestOpenDir(u32 path, u32 fd_out);
    s32 GuestReadDir(u32 fd, u32 dirent_out, u32 nread_out);
    s32 GuestPathOp(u32 path, s32 (FsService::*op)(std::string_view));
    s32 GuestRename(u32 from, u32 to);
    s32 GuestTruncate(u32 path, u64 size);
    s32 GuestGetBlockSize(u32 path, u32 sector_out, u32 block_out);
    s32 GuestFGetBlockSize(u32 fd, u32 sector_out, u32 block_out);
    s32 GuestGetFreeSize(u32 path, u32 block_size_out /* u32* */, u32 block_count_out /* u64* */);

    void RegisterSyscalls(SyscallTable& table);
    // Open fds, I/O totals and recently missing paths (end report / stall report).
    [[nodiscard]] std::vector<std::string> Describe() const;
    [[nodiscard]] u64 total_bytes_read() const noexcept { return bytes_read_.load(); }

private:
    s32 Resolve(std::string_view path, VfsResolved* out) const;
    // Reads a guest path (NUL terminated, <= kFsMaxPath); CELL_* on failure.
    s32 ReadPath(u32 ea, std::string* out) const;
    std::shared_ptr<FsHandle> Get(u32 fd, FsHandle::Kind kind) const;
    // I/O on a handle already looked up (the caller keeps it alive and, for the sequential
    // calls, holds its mutex): never re-resolves the fd, which may be closed and reused.
    s32 DoReadAt(FsHandle& h, u64 offset, u32 buffer, u64 size, u64* done);
    s32 DoWriteAt(FsHandle& h, u64 offset, u32 buffer, u64 size, u64* done);
    s32 Insert(std::shared_ptr<FsHandle> handle, u32* fd);
    void NoteMissing(std::string_view path);
    void InvalidateCode(u32 ea, u64 size);
    FsStat StatOf(const std::filesystem::path& host, const VfsMount* mount, s32* error) const;

    Kernel& kernel_;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<FsHandle>> fds_ = std::vector<std::shared_ptr<FsHandle>>(kFsFdLimit);
    std::deque<std::string> missing_;  // most recent last, unique
    std::atomic<u64> opens_{0}, open_failures_{0}, bytes_read_{0}, bytes_written_{0}, reads_{0};
};

void WriteFsStat(vm::GuestMemory& memory, u32 ea, const FsStat& st);
void WriteFsDirent(vm::GuestMemory& memory, u32 ea, const FsDirent& d);

}  // namespace cell::lv2
