// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_mmapper.cpp
#include "lv2/sys_mmapper.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "common/log.h"
#include "lv2/sys_event.h"
#include "lv2/sys_memory.h"
#include "lv2/sys_ppu_thread.h"

namespace cell::lv2 {

namespace {

// 0 means the default (1 MiB pages); anything but 64K / 1M is invalid.
bool PageSizeFrom(u64 flags, u32* page) {
    switch (flags & SYS_MEMORY_PAGE_SIZE_MASK) {
    case 0:
    case SYS_MEMORY_PAGE_SIZE_1M: *page = 0x100000; return true;
    case SYS_MEMORY_PAGE_SIZE_64K: *page = 0x10000; return true;
    default: return false;
    }
}

u8 ProtFrom(u64 flags) { return (flags & SYS_MEMORY_PROT_READ_ONLY) ? vm::kProtRead : vm::kProtReadWrite; }

}  // namespace

MmapperService::~MmapperService() = default;

std::pair<const u32, MmapperService::AddressArea>* MmapperService::FindAreaLocked(u32 addr, u64 size) {
    auto it = areas_.upper_bound(addr);
    if (it == areas_.begin()) return nullptr;
    --it;
    if (u64{addr} + size > u64{it->first} + it->second.size) return nullptr;
    return &*it;
}

s32 MmapperService::AllocateAddress(u64 size, u64 flags, u64 alignment, u32* out_addr) {
    if (!size || size % kMmapperGranule) return CELL_EALIGN;
    if (!alignment) alignment = kMmapperGranule;
    if (alignment < kMmapperGranule || alignment > 0x80000000ull || (alignment & (alignment - 1))) return CELL_EALIGN;
    u32 page = 0;
    if (!PageSizeFrom(flags, &page)) return CELL_EINVAL;
    std::lock_guard lock(mutex_);
    const auto addr = kernel_.memory().ReserveRange(vm::Area::Mmapper, size, static_cast<u32>(alignment));
    if (!addr) return CELL_ENOMEM;
    areas_[*addr] = AddressArea{size, page, {}};
    *out_addr = *addr;
    LOG_INFO("lv2", "sys_mmapper_allocate_address(0x%llx, page 0x%x) -> 0x%08x", static_cast<unsigned long long>(size),
             page, *addr);
    return CELL_OK;
}

s32 MmapperService::AllocateFixedAddress() {
    std::lock_guard lock(mutex_);
    if (areas_.count(kMmapperFixedAddress)) return CELL_EEXIST;
    if (!kernel_.memory().ReserveFixedRange(vm::Area::Mmapper, kMmapperFixedAddress, kMmapperGranule))
        return CELL_EEXIST;
    areas_[kMmapperFixedAddress] = AddressArea{kMmapperGranule, 0x100000, {}};
    return CELL_OK;
}

s32 MmapperService::FreeAddress(u32 addr) {
    std::lock_guard lock(mutex_);
    const auto it = areas_.find(addr);
    if (it == areas_.end()) return CELL_EINVAL;
    if (!it->second.mappings.empty()) return CELL_EBUSY;
    {
        // A thread still suspended on a page of this area keeps it alive.
        std::lock_guard pf(pf_mutex_);
        for (const auto& [key, f] : faults_)
            if (!f.recovered && f.addr >= addr && u64{f.addr} < u64{addr} + it->second.size) return CELL_EBUSY;
    }
    notify_.erase(addr);
    kernel_.memory().Unreserve(vm::Area::Mmapper, addr);
    areas_.erase(it);
    return CELL_OK;
}

s32 MmapperService::AllocateMemory(u64 key, u64 size, u64 flags, u32* out_id) {
    u32 page = 0;
    if (!PageSizeFrom(flags, &page)) return CELL_EINVAL;
    if (!size || size % page) return CELL_EALIGN;
    if (size > 0xFFFFFFFFull) return CELL_ENOMEM;
    if (!kernel_.memory().Charge(size)) return CELL_ENOMEM;
    if (key != SYS_MMAPPER_NO_SHM_KEY)
        LOG_WARN("lv2", "sys_mmapper: IPC key 0x%llx treated as process-private memory", static_cast<unsigned long long>(key));
    auto mem = std::make_shared<Lv2Memory>();
    mem->size = size;
    mem->page_size = page;
    mem->key = key;
    *out_id = kernel_.objects().Add(kIdMemory, mem);
    LOG_INFO("lv2", "sys_mmapper_allocate_shared_memory(0x%llx, page 0x%x) -> id 0x%08x",
             static_cast<unsigned long long>(size), page, *out_id);
    return CELL_OK;
}

s32 MmapperService::FreeMemory(u32 id) {
    const auto mem = kernel_.objects().Get<Lv2Memory>(id);
    if (!mem) return CELL_ESRCH;
    std::lock_guard lock(mutex_);
    if (mem->mapped_at) return CELL_EBUSY;
    kernel_.objects().Remove(id);
    kernel_.memory().Uncharge(mem->size);
    return CELL_OK;
}

s32 MmapperService::MapLocked(u32 addr, Lv2Memory& mem, u64 flags) {
    if (addr % mem.page_size) return CELL_EALIGN;
    auto* area = FindAreaLocked(addr, mem.size);
    if (!area) return CELL_EINVAL;
    if (mem.mapped_at) return CELL_EBUSY;
    // Overlap with an existing mapping of the same area?
    for (const auto& [at, other_id] : area->second.mappings) {
        const auto other = kernel_.objects().Get<Lv2Memory>(other_id);
        const u64 other_size = other ? other->size : 0;
        if (u64{addr} < at + other_size && u64{at} < addr + mem.size) return CELL_EBUSY;
    }
    vm::GuestMemory& m = kernel_.memory();
    if (!m.CommitRange(addr, mem.size, ProtFrom(flags))) return CELL_ENOMEM;
    if (!mem.saved.empty()) {
        std::memcpy(m.shadow() + addr, mem.saved.data(), mem.saved.size());
        mem.saved.clear();
        mem.saved.shrink_to_fit();
    }
    mem.mapped_at = addr;
    area->second.mappings[addr] = mem.id;
    return CELL_OK;
}

s32 MmapperService::Map(u32 addr, u32 id, u64 flags) {
    const auto mem = kernel_.objects().Get<Lv2Memory>(id);
    if (!mem) return CELL_ESRCH;
    std::lock_guard lock(mutex_);
    const s32 r = MapLocked(addr, *mem, flags);
    if (r == CELL_OK)
        LOG_INFO("lv2", "sys_mmapper_map_shared_memory(0x%08x, id 0x%08x, 0x%llx bytes)", addr, id,
                 static_cast<unsigned long long>(mem->size));
    return r;
}

s32 MmapperService::SearchAndMap(u32 start_addr, u32 id, u64 flags, u32* out_addr) {
    const auto mem = kernel_.objects().Get<Lv2Memory>(id);
    if (!mem) return CELL_ESRCH;
    std::lock_guard lock(mutex_);
    const auto it = areas_.find(start_addr);
    if (it == areas_.end()) return CELL_EINVAL;
    for (u64 at = start_addr; at + mem->size <= u64{start_addr} + it->second.size; at += mem->page_size) {
        const s32 r = MapLocked(static_cast<u32>(at), *mem, flags);
        if (r == CELL_OK) {
            *out_addr = static_cast<u32>(at);
            return CELL_OK;
        }
        if (r != CELL_EBUSY || mem->mapped_at) return r;
    }
    return CELL_ENOMEM;
}

s32 MmapperService::Unmap(u32 addr, u32* out_id) {
    std::lock_guard lock(mutex_);
    auto* area = FindAreaLocked(addr, 1);
    if (!area) return CELL_EINVAL;
    const auto it = area->second.mappings.find(addr);
    if (it == area->second.mappings.end()) return CELL_EINVAL;
    const u32 id = it->second;
    const auto mem = kernel_.objects().Get<Lv2Memory>(id);
    vm::GuestMemory& m = kernel_.memory();
    if (mem) {
        // Keep the contents for a later map (skipped when the object is all zero).
        const u8* src = m.shadow() + addr;
        bool zero = true;
        for (u64 i = 0; i < mem->size && zero; i += 8) {
            u64 v;
            std::memcpy(&v, src + i, 8);
            zero = v == 0;
        }
        if (!zero) mem->saved.assign(src, src + mem->size);
        m.DecommitRange(addr, mem->size);
        mem->mapped_at = 0;
    }
    area->second.mappings.erase(it);
    *out_id = id;
    return CELL_OK;
}

// ---------------------------------------------------------------------------------------
// Demand paging
// ---------------------------------------------------------------------------------------

s32 MmapperService::EnablePageFaultNotification(u32 start_addr, u32 queue_id) {
    const auto queue = kernel_.objects().Get<Lv2EventQueue>(queue_id);
    std::lock_guard lock(mutex_);
    if (!areas_.count(start_addr)) return CELL_EINVAL;  // must be the base of an allocated area
    if (!queue) return CELL_ESRCH;
    auto& slot = notify_[start_addr];
    if (!slot.expired()) return CELL_EBUSY;
    slot = queue;
    LOG_INFO("lv2", "sys_mmapper_enable_page_fault_notification(0x%08x, queue 0x%08x): demand paging on", start_addr, queue_id);
    return CELL_OK;
}

bool MmapperService::HandlePageFault(u64 type, u32 thread_id, u32 ea, bool write, const std::atomic<bool>* cancel) {
    std::shared_ptr<Lv2EventQueue> queue;
    u32 area_base = 0;
    {
        std::lock_guard lock(mutex_);
        const auto* area = FindAreaLocked(ea, 1);
        if (!area) return false;  // not in an mmapper area: an ordinary fault
        area_base = area->first;
        const auto it = notify_.find(area_base);
        if (it != notify_.end()) queue = it->second.lock();
        if (!queue) {
            LOG_ERROR("lv2", "%s fault at 0x%08x inside sys_mmapper area 0x%08x: nothing mapped there and no page-fault "
                      "notification registered (sys_mmapper_enable_page_fault_notification)",
                      write ? "write" : "read", ea, area_base);
            return false;
        }
    }
    vm::GuestMemory& m = kernel_.memory();
    // Mapped meanwhile (another thread's fault was served first): just retry.
    if ((m.Flags(ea) & (write ? vm::kPageWritable : vm::kPageReadable)) == (write ? vm::kPageWritable : vm::kPageReadable))
        return true;
    const u64 cause = (m.Flags(ea) & vm::kPageMapped) ? SYS_MEMORY_PAGE_FAULT_CAUSE_READ_ONLY : SYS_MEMORY_PAGE_FAULT_CAUSE_NON_MAPPED;
    const u64 key = FaultKey(type, thread_id);
    {
        std::lock_guard pf(pf_mutex_);
        faults_[key] = PendingFault{ea, false};
    }
    faults_total_.fetch_add(1, std::memory_order_relaxed);
    LOG_TRACE("lv2", "page fault: %s thread 0x%08x %s 0x%08x (%s) -> pager queue 0x%08x", type ? "SPU" : "PPU", thread_id,
              write ? "write" : "read", ea, cause == SYS_MEMORY_PAGE_FAULT_CAUSE_NON_MAPPED ? "not mapped" : "read-only", queue->id);
    const s32 sent = queue->Push({SYS_MEMORY_PAGE_FAULT_EVENT_KEY, ea, key, cause});
    if (sent != CELL_OK) {
        LOG_ERROR("lv2", "page fault at 0x%08x: pager queue 0x%08x refused the event (0x%08x)", ea, queue->id, static_cast<u32>(sent));
        std::lock_guard pf(pf_mutex_);
        faults_.erase(key);
        return false;
    }
    std::unique_lock pf(pf_mutex_);
    const WaitResult r = kernel_.Wait(pf, pf_cv_, 0, [&] {
        const auto it = faults_.find(key);
        return it == faults_.end() || it->second.recovered || (cancel && cancel->load(std::memory_order_acquire));
    });
    faults_.erase(key);
    return r == WaitResult::Ready && !(cancel && cancel->load(std::memory_order_acquire));
}

s32 MmapperService::RecoverPageFault(u64 type, u32 thread_id) {
    std::lock_guard pf(pf_mutex_);
    const auto it = faults_.find(FaultKey(type, thread_id));
    if (it == faults_.end() || it->second.recovered) return CELL_EINVAL;  // that thread is not page-faulted
    it->second.recovered = true;
    pf_cv_.notify_all();
    return CELL_OK;
}

bool MmapperService::IsPageFaulted(u64 type, u32 thread_id) const {
    std::lock_guard pf(pf_mutex_);
    const auto it = faults_.find(FaultKey(type, thread_id));
    return it != faults_.end() && !it->second.recovered;
}

void MmapperService::WakeForTermination() {
    std::lock_guard pf(pf_mutex_);
    pf_cv_.notify_all();
}

std::vector<std::string> MmapperService::Describe() const {
    std::vector<std::string> out;
    char line[160];
    std::lock_guard pf(pf_mutex_);
    std::snprintf(line, sizeof(line), "%" PRIu64 " page fault(s) sent to the guest pager", faults_total_.load());
    out.emplace_back(line);
    for (const auto& [key, f] : faults_) {
        std::snprintf(line, sizeof(line), "%s thread 0x%08x suspended on 0x%08x%s", (key >> 32) ? "SPU" : "PPU",
                      static_cast<u32>(key), f.addr, f.recovered ? " (recovered, resuming)" : " (waiting for recover_page_fault)");
        out.emplace_back(line);
    }
    return out;
}

void MmapperService::RegisterSyscalls(SyscallTable& table) {
    using C = ppu::PpuThreadContext;
    static constexpr auto S = [](void* p) -> MmapperService& { return *static_cast<MmapperService*>(p); };
    static constexpr auto A32 = [](const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); };
    // Writes a u32 result through a guest pointer; EFAULT when it is not writable.
    static constexpr auto Out = [](MmapperService& s, u32 ptr, u32 value, s32 result) -> s32 {
        if (result != CELL_OK) return result;
        if (!s.kernel_.memory().Check(ptr, 4, vm::kPageWritable)) return CELL_EFAULT;
        s.kernel_.memory().Write<u32>(ptr, value);
        return CELL_OK;
    };

    table.Register(
        327, "sys_mmapper_enable_page_fault_notification",
        [](void* p, C& c) -> s32 { return S(p).EnablePageFaultNotification(A32(c, 0), A32(c, 1)); }, this);
    // 57 sys_ppu_thread_recover_page_fault(thread_id)
    table.Register(
        57, "sys_ppu_thread_recover_page_fault",
        [](void* p, C& c) -> s32 {
            MmapperService& s = S(p);
            const u32 id = A32(c, 0);
            const s32 r = s.RecoverPageFault(SYS_MEMORY_PAGE_FAULT_TYPE_PPU_THREAD, id);
            if (r == CELL_OK) return r;
            for (const auto& t : s.kernel_.threads().Snapshot())
                if (t.id == id) return CELL_EINVAL;  // exists, but is not page-faulted
            return CELL_ESRCH;
        },
        this);
    // 198 sys_spu_thread_recover_page_fault(spu_thread_id)
    table.Register(
        198, "sys_spu_thread_recover_page_fault",
        [](void* p, C& c) -> s32 { return S(p).RecoverPageFault(SYS_MEMORY_PAGE_FAULT_TYPE_SPU_THREAD, A32(c, 0)); }, this);
    table.Register(326, "sys_mmapper_allocate_fixed_address",
                   [](void* p, C&) -> s32 { return S(p).AllocateFixedAddress(); }, this);
    table.Register(329, "sys_mmapper_free_shared_memory", [](void* p, C& c) -> s32 { return S(p).FreeMemory(A32(c, 0)); },
                   this);
    table.Register(
        330, "sys_mmapper_allocate_address",
        [](void* p, C& c) -> s32 {
            MmapperService& s = S(p);
            if (!s.kernel_.memory().Check(A32(c, 3), 4, vm::kPageWritable)) return CELL_EFAULT;
            u32 addr = 0;
            const s32 r = s.AllocateAddress(c.gpr[3], c.gpr[4], c.gpr[5], &addr);
            return Out(s, A32(c, 3), addr, r);
        },
        this);
    table.Register(331, "sys_mmapper_free_address", [](void* p, C& c) -> s32 { return S(p).FreeAddress(A32(c, 0)); },
                   this);
    table.Register(
        332, "sys_mmapper_allocate_shared_memory",
        [](void* p, C& c) -> s32 {
            MmapperService& s = S(p);
            if (!s.kernel_.memory().Check(A32(c, 3), 4, vm::kPageWritable)) return CELL_EFAULT;
            u32 id = 0;
            const s32 r = s.AllocateMemory(c.gpr[3], c.gpr[4], c.gpr[5], &id);
            return Out(s, A32(c, 3), id, r);
        },
        this);
    table.Register(
        334, "sys_mmapper_map_shared_memory",
        [](void* p, C& c) -> s32 { return S(p).Map(A32(c, 0), A32(c, 1), c.gpr[5]); }, this);
    table.Register(
        335, "sys_mmapper_unmap_shared_memory",
        [](void* p, C& c) -> s32 {
            MmapperService& s = S(p);
            if (!s.kernel_.memory().Check(A32(c, 1), 4, vm::kPageWritable)) return CELL_EFAULT;
            u32 id = 0;
            const s32 r = s.Unmap(A32(c, 0), &id);
            return Out(s, A32(c, 1), id, r);
        },
        this);
    table.Register(
        337, "sys_mmapper_search_and_map",
        [](void* p, C& c) -> s32 {
            MmapperService& s = S(p);
            if (!s.kernel_.memory().Check(A32(c, 3), 4, vm::kPageWritable)) return CELL_EFAULT;
            u32 addr = 0;
            const s32 r = s.SearchAndMap(A32(c, 0), A32(c, 1), c.gpr[5], &addr);
            return Out(s, A32(c, 3), addr, r);
        },
        this);
}

}  // namespace cell::lv2
