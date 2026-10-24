// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_mmapper.h
//
// lv2 memory mapper: the PS3's two-step "reserve an address range, then map memory objects
// into it" API (closest PS4 analogue: sceKernelReserveVirtualRange + sceKernelMapDirectMemory).
// Syscalls (lv2 numbering, as in RPCS3's table):
//   326 sys_mmapper_allocate_fixed_address()                     reserves 0xB0000000 (256 MiB)
//   329 sys_mmapper_free_shared_memory(mem_id)
//   330 sys_mmapper_allocate_address(size, flags, alignment, u32* addr)
//   331 sys_mmapper_free_address(addr)
//   332 sys_mmapper_allocate_shared_memory(ipc_key, size, flags, u32* mem_id)
//   334 sys_mmapper_map_shared_memory(addr, mem_id, flags)
//   335 sys_mmapper_unmap_shared_memory(addr, u32* mem_id)
//   337 sys_mmapper_search_and_map(start_addr, mem_id, flags, u32* addr)
//   327 sys_mmapper_enable_page_fault_notification(start_addr, event_queue_id)
//    57 sys_ppu_thread_recover_page_fault(thread_id)
//   198 sys_spu_thread_recover_page_fault(spu_thread_id)
// sysPrxForUser wraps them as sys_mmapper_{allocate,map,unmap,free}_memory (HLE, same service).
//
// PC mapping: an address area is a reservation of 256 MiB granules in the arena's mmapper
// region (0x40000000-0xBFFFFFFF), inaccessible until something is mapped. A memory object is
// charged to the 213 MiB user budget at allocation; mapping commits its pages at the target
// address. Its contents survive unmap/remap (kept in a host buffer while unmapped). A memory
// object is mapped at most once at a time (EBUSY otherwise).
//
// Demand paging (page-fault notification). A game may reserve a large address area, map
// nothing, and let its own pager thread back pages on first touch:
//   1. sys_mmapper_enable_page_fault_notification(area, queue): faults inside `area` become
//      events on `queue` instead of killing the process;
//   2. a PPU thread (load/store, lwarx/stwcx.) or an SPU thread (MFC GET/PUT/GETLLAR/PUTLLC)
//      touches an unmapped (or read-only) page there: the access is suspended — the thread
//      blocks inside the instruction / DMA — and the event
//          source = SYS_MEMORY_PAGE_FAULT_EVENT_KEY (0xFFFE000000000000)
//          data1  = faulting address
//          data2  = (type << 32) | thread id   type 0 PPU thread, 1 SPU thread
//          data3  = cause                       2 not mapped, 1 read-only
//      is pushed to the queue;
//   3. the guest pager maps memory there (sys_mmapper_map_shared_memory...) and calls
//      sys_ppu_thread_recover_page_fault / sys_spu_thread_recover_page_fault on the thread;
//   4. the access is retried; if the page is still not accessible the cycle repeats.
// This replaces the host-side SIGSEGV/VEH trap a JIT would need: the interpreters check every
// access already (vm::GuestMemory page flags), so the fault is caught before it happens and the
// suspended access needs no context unwinding. Semantics and constants follow RPCS3.
// Auto-committing the area instead would break the protocol: the pager's later map at that
// address would fail (EBUSY) and any data it streams into the page would be lost.
#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <string>
#include <memory>
#include <mutex>
#include <vector>

#include "lv2/kernel.h"

namespace cell::lv2 {

inline constexpr u32 kIdMemory = 0x08000000;
inline constexpr u64 SYS_MMAPPER_NO_SHM_KEY = 0xFFFF000000000000ull;
inline constexpr u64 SYS_MEMORY_PROT_READ_ONLY = 0x80000;
inline constexpr u64 SYS_MEMORY_PROT_READ_WRITE = 0x40000;
inline constexpr u64 SYS_MEMORY_PAGE_SIZE_MASK = 0xF00;
inline constexpr u32 kMmapperGranule = 0x10000000;
inline constexpr u32 kMmapperFixedAddress = 0xB0000000;

inline constexpr u64 SYS_MEMORY_PAGE_FAULT_EVENT_KEY = 0xFFFE000000000000ull;
inline constexpr u64 SYS_MEMORY_PAGE_FAULT_CAUSE_READ_ONLY = 0x1;
inline constexpr u64 SYS_MEMORY_PAGE_FAULT_CAUSE_NON_MAPPED = 0x2;
inline constexpr u64 SYS_MEMORY_PAGE_FAULT_TYPE_PPU_THREAD = 0x0;
inline constexpr u64 SYS_MEMORY_PAGE_FAULT_TYPE_SPU_THREAD = 0x1;

struct Lv2EventQueue;

struct Lv2Memory : Lv2Object {
    u64 size = 0;
    u32 page_size = 0x100000;
    u64 key = SYS_MMAPPER_NO_SHM_KEY;
    u32 mapped_at = 0;     // 0 = not mapped (guarded by MmapperService's mutex)
    std::vector<u8> saved; // contents while unmapped (empty = all zero)
};

class MmapperService {
public:
    explicit MmapperService(Kernel& kernel) noexcept : kernel_(kernel) {}
    ~MmapperService();

    s32 AllocateAddress(u64 size, u64 flags, u64 alignment, u32* out_addr);
    s32 AllocateFixedAddress();
    s32 FreeAddress(u32 addr);
    s32 AllocateMemory(u64 key, u64 size, u64 flags, u32* out_id);
    s32 FreeMemory(u32 id);
    s32 Map(u32 addr, u32 id, u64 flags);
    s32 SearchAndMap(u32 start_addr, u32 id, u64 flags, u32* out_addr);
    s32 Unmap(u32 addr, u32* out_id);

    // ---- demand paging ----
    s32 EnablePageFaultNotification(u32 start_addr, u32 queue_id);
    // Called on the faulting thread. Blocks until the guest recovers the thread (true: retry
    // the access), or returns false at once when no notification covers `ea` (a real fault)
    // or when the process ends / `cancel` is raised while waiting.
    bool HandlePageFault(u64 type, u32 thread_id, u32 ea, bool write, const std::atomic<bool>* cancel = nullptr);
    s32 RecoverPageFault(u64 type, u32 thread_id);
    [[nodiscard]] bool IsPageFaulted(u64 type, u32 thread_id) const;
    void WakeForTermination();
    [[nodiscard]] std::vector<std::string> Describe() const;

    void RegisterSyscalls(SyscallTable& table);

private:
    struct AddressArea {
        u64 size = 0;
        u32 page_size = 0x100000;
        std::map<u32, u32> mappings;  // address -> memory id
    };
    // Area containing [addr, addr+size), or nullptr. mutex_ held.
    std::pair<const u32, AddressArea>* FindAreaLocked(u32 addr, u64 size);
    s32 MapLocked(u32 addr, Lv2Memory& mem, u64 flags);

    struct PendingFault {
        u32 addr = 0;
        bool recovered = false;
    };
    static u64 FaultKey(u64 type, u32 id) noexcept { return (type << 32) | id; }

    Kernel& kernel_;
    std::mutex mutex_;
    std::map<u32, AddressArea> areas_;  // base -> area
    std::map<u32, std::weak_ptr<Lv2EventQueue>> notify_;  // area base -> pager queue (mutex_)
    // Faulted threads (pf_mutex_). Never held together with mutex_ or a queue mutex.
    mutable std::mutex pf_mutex_;
    std::condition_variable pf_cv_;
    std::map<u64, PendingFault> faults_;
    std::atomic<u64> faults_total_{0};
};

}  // namespace cell::lv2
