// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_memory.cpp
#include "lv2/sys_memory.h"

#include "common/log.h"

namespace cell::lv2 {

s32 MemoryService::Allocate(u64 size, u64 flags, u32 out_addr_ptr) {
    // The page-size flag picks both the alignment and the area. Unlike
    // sceKernelAllocateDirectMemory (any 16 KiB multiple, caller-chosen alignment), lv2
    // only knows two page sizes and rejects anything else.
    u32 page;
    vm::Area area;
    switch (flags) {
    case SYS_MEMORY_PAGE_SIZE_1M:
        page = vm::k1M;
        area = vm::Area::User1M;
        break;
    case SYS_MEMORY_PAGE_SIZE_64K:
        page = vm::k64K;
        area = vm::Area::User64K;
        break;
    default:
        return CELL_EINVAL;
    }
    // Size must be a whole number of pages (lv2: CELL_EALIGN). Zero is treated the same.
    if (!size || size % page) return CELL_EALIGN;
    if (size > vm::Describe(area).size) return CELL_ENOMEM;
    // The out pointer is a 32-bit guest address the kernel writes big-endian.
    if (!memory_.Check(out_addr_ptr, sizeof(u32), vm::kPageWritable)) return CELL_EFAULT;

    // Budget first (sys_memory_get_user_memory_size must stay truthful), then address space.
    if (!memory_.Charge(size)) return CELL_ENOMEM;
    const auto addr = memory_.Allocate(area, size, page, vm::kProtReadWrite);
    if (!addr) {
        memory_.Uncharge(size);
        return CELL_ENOMEM;
    }
    {
        std::lock_guard lock(mutex_);
        blocks_.emplace(*addr, Block{size, area});
    }
    memory_.Write<u32>(out_addr_ptr, *addr);
    LOG_TRACE("lv2", "sys_memory_allocate(0x%llx, %s) -> 0x%08x", static_cast<unsigned long long>(size),
              page == vm::k1M ? "1M" : "64K", *addr);
    return CELL_OK;
}

s32 MemoryService::Free(u32 addr) {
    Block block;
    {
        std::lock_guard lock(mutex_);
        const auto it = blocks_.find(addr);
        if (it == blocks_.end()) return CELL_EINVAL;  // only the base of an allocation
        block = it->second;
        blocks_.erase(it);
    }
    memory_.Free(block.area, addr);
    memory_.Uncharge(block.size);
    return CELL_OK;
}

s32 MemoryService::GetUserMemorySize(u32 info_ptr) {
    if (!memory_.Check(info_ptr, sizeof(sys_memory_info_t), vm::kPageWritable)) return CELL_EFAULT;
    sys_memory_info_t info;
    info.total_user_memory = static_cast<u32>(memory_.user_budget());
    info.available_user_memory = static_cast<u32>(memory_.user_budget() - memory_.user_used());
    memory_.CopyIn(info_ptr, &info, sizeof(info));
    return CELL_OK;
}

void MemoryService::RegisterSyscalls(SyscallTable& table) {
    // Thin adapters: unpack r3..r5 (only the low 32 bits of pointers are meaningful).
    table.Register(
        SYS_MEMORY_ALLOCATE, "sys_memory_allocate",
        [](void* self, ppu::PpuThreadContext& ctx) -> s32 {
            return static_cast<MemoryService*>(self)->Allocate(ctx.gpr[3], ctx.gpr[4],
                                                               static_cast<u32>(ctx.gpr[5]));
        },
        this);
    table.Register(
        SYS_MEMORY_FREE, "sys_memory_free",
        [](void* self, ppu::PpuThreadContext& ctx) -> s32 {
            return static_cast<MemoryService*>(self)->Free(static_cast<u32>(ctx.gpr[3]));
        },
        this);
    table.Register(
        SYS_MEMORY_GET_USER_MEMORY_SIZE, "sys_memory_get_user_memory_size",
        [](void* self, ppu::PpuThreadContext& ctx) -> s32 {
            return static_cast<MemoryService*>(self)->GetUserMemorySize(static_cast<u32>(ctx.gpr[3]));
        },
        this);
}

}  // namespace cell::lv2
