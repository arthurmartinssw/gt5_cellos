// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — common/host_file.h
//
// Host file handle for the guest file system (cellFs / sys_fs). Requirements that rule out
// std::fstream / FILE*:
//   * 64-bit offsets everywhere: a game's volume archive (GT5's GT.VOL) is several GiB, and
//     MSVC's fseek/ftell are 32-bit;
//   * positional I/O only (ReadAt/WriteAt, no shared file pointer). lv2 keeps one position
//     per guest fd, cellFsReadWithOffset must not move it, and several PPU threads may read
//     the same archive at once: pread/pwrite (POSIX) and ReadFile/WriteFile with an
//     OVERLAPPED offset (Windows) are safe to call concurrently on one handle;
//   * reads straight into guest memory (the arena's shadow view), no bounce buffer.
// OS calls are isolated below, behind #ifdef _WIN32 / POSIX.
#pragma once

#include <filesystem>
#include <optional>

#include "common/types.h"

namespace cell::host {

enum class FileError : u8 { None, NotFound, Exists, Access, IsDir, NotDir, NoSpace, TooMany, NameTooLong, Io };

// Open flags (combine). Read/Write select the access; the others need Write except Create.
enum FileOpen : u32 {
    kFileRead = 1,
    kFileWrite = 2,
    kFileCreate = 4,     // create if missing
    kFileExclusive = 8,  // with kFileCreate: fail with Exists if present
    kFileTruncate = 16,  // size 0 on open
};

class File {
public:
    File() noexcept = default;
    ~File();
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    [[nodiscard]] static std::optional<File> Open(const std::filesystem::path& path, u32 flags, FileError* error);

    [[nodiscard]] bool is_open() const noexcept;
    // Read/write `size` bytes at `offset`. Short counts only at end of file (read) or on
    // error; `*done` always holds what was transferred. Thread-safe on one handle.
    FileError ReadAt(u64 offset, void* buffer, u64 size, u64* done) const;
    FileError WriteAt(u64 offset, const void* buffer, u64 size, u64* done) const;
    [[nodiscard]] std::optional<u64> Size() const;
    FileError Truncate(u64 size) const;
    FileError Sync() const;
    void Close() noexcept;

private:
#ifdef _WIN32
    void* handle_ = nullptr;  // HANDLE; nullptr = closed (INVALID_HANDLE_VALUE never stored)
#else
    int fd_ = -1;
#endif
};

}  // namespace cell::host
