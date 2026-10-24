// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_fs.h
//
// cellFs (import library "sys_fs"): the file API games link against. Each export maps to the
// lv2 file system service (lv2/sys_fs.h) — the same code the sys_fs_* syscalls use — so the
// HLE layer here is argument shuffling only. Signatures (SDK cell/fs/cell_fs_file_api.h):
//   cellFsOpen(const char* path, int flags, int* fd, const void* arg, uint64_t size)
//   cellFsRead(int fd, void* buf, uint64_t nbytes, uint64_t* nread)
//   cellFsWrite(int fd, const void* buf, uint64_t nbytes, uint64_t* nwrite)
//   cellFsClose(int fd)
//   cellFsLseek(int fd, int64_t offset, int whence, uint64_t* pos)
//   cellFsReadWithOffset(int fd, uint64_t offset, void* buf, uint64_t size, uint64_t* nread)
//   cellFsWriteWithOffset(int fd, uint64_t offset, const void* buf, uint64_t size, uint64_t* nwrite)
//   cellFsStat(const char* path, CellFsStat* sb)      cellFsFstat(int fd, CellFsStat* sb)
//   cellFsOpendir(const char* path, int* fd)          cellFsReaddir(int fd, CellFsDirent* d, uint64_t* nread)
//   cellFsClosedir(int fd)
//   cellFsMkdir(path, mode)  cellFsRmdir(path)  cellFsUnlink(path)  cellFsRename(from, to)
//   cellFsTruncate(path, size)  cellFsFtruncate(fd, size)  cellFsFsync(fd)
//   cellFsGetBlockSize(path, u64* sector, u64* block)  cellFsFGetBlockSize(fd, u64*, u64*)
//   cellFsGetFreeSize(path, u32* block_size, u64* block_count)
// Accepted without effect: cellFsChmod, cellFsUtime (host permissions/times are not mapped),
// cellFsSetDefaultContainer / cellFsSetIoBuffer* (our reads need no lv2 I/O buffer).
// Still logging stubs: cellFsAio*, cellFsSt* (streaming), cellFsSdata*, cellFsGetDirectoryEntries.
#pragma once

#include "hle/hle_registry.h"
#include "lv2/kernel.h"

namespace cell::hle {

class CellFs {
public:
    explicit CellFs(lv2::Kernel& kernel) noexcept : kernel_(kernel) {}
    void Register(HleRegistry& registry);
    lv2::Kernel& kernel() noexcept { return kernel_; }

private:
    lv2::Kernel& kernel_;
};

}  // namespace cell::hle
