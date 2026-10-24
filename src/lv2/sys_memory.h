// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_memory.h
//
// lv2 user-memory syscalls: the PS3 replacement for bloodborne_pc's direct/flexible memory.
//
//   PS4 call (runtime_memory.c)               what it does on Orbis              PS3 equivalent
//   ----------------------------------------  ---------------------------------  ------------------------------
//   sceKernelGetDirectMemorySize              size of the physical pool          sys_memory_get_user_memory_size (352)
//                                              (5056 MiB retail)                  (213 MiB total / available)
//   sceKernelAllocateDirectMemory             reserve physical range, no VA      no split: sys_memory_allocate (348)
//   + sceKernelMapDirectMemory                then map it at a VA                 allocates *and* maps in one call,
//   sceKernelMapFlexibleMemory / mmap(ANON)   anonymous memory for malloc         64 KiB or 1 MiB pages
//   sceKernelReleaseDirectMemory / munmap     unmap + release                     sys_memory_free (349)
//   sceKernelMprotect                         change protection                   no general mprotect for games;
//                                                                                 sys_memory_get_page_attribute (351)
//                                                                                 only queries (later phase)
//   sceKernelReserveVirtualRange              VA reservation                     sys_mmapper_allocate_address (330,
//                                                                                 later phase; area Mmapper)
#pragma once

#include <mutex>
#include <unordered_map>

#include "common/types.h"
#include "lv2/syscall_table.h"
#include "memory/guest_memory.h"

namespace cell::lv2 {

inline constexpr u64 SYS_MEMORY_PAGE_SIZE_64K = 0x200;
inline constexpr u64 SYS_MEMORY_PAGE_SIZE_1M = 0x400;

inline constexpr u32 SYS_MEMORY_ALLOCATE = 348;
inline constexpr u32 SYS_MEMORY_FREE = 349;
inline constexpr u32 SYS_MEMORY_GET_USER_MEMORY_SIZE = 352;

// Guest layout, written into PPU memory.
struct sys_memory_info_t {
    be_t<u32> total_user_memory;
    be_t<u32> available_user_memory;
};
static_assert(sizeof(sys_memory_info_t) == 8);

class MemoryService {
public:
    explicit MemoryService(vm::GuestMemory& memory) noexcept : memory_(memory) {}

    // sys_memory_allocate(size, flags, u32* alloc_addr)
    s32 Allocate(u64 size, u64 flags, u32 out_addr_ptr);
    // sys_memory_free(addr)
    s32 Free(u32 addr);
    // sys_memory_get_user_memory_size(sys_memory_info_t* info)
    s32 GetUserMemorySize(u32 info_ptr);

    void RegisterSyscalls(SyscallTable& table);

private:
    struct Block {
        u64 size;
        vm::Area area;
    };
    vm::GuestMemory& memory_;
    std::mutex mutex_;
    std::unordered_map<u32, Block> blocks_;
};

}  // namespace cell::lv2
