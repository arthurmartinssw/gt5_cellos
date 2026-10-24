// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/kernel.cpp
#include "lv2/kernel.h"

#include <thread>

#include "common/log.h"
#include "lv2/sys_event.h"
#include "lv2/sys_fs.h"
#include "lv2/sys_mmapper.h"
#include "lv2/sys_prx.h"
#include "lv2/sys_ppu_thread.h"
#include "lv2/sys_spu.h"
#include "lv2/sys_sync.h"
#include "lv2/sys_timer.h"

namespace cell::lv2 {

namespace {
thread_local u32 tls_current_thread_id = 0;
}

u32 Kernel::CurrentThreadId() noexcept { return tls_current_thread_id; }
void Kernel::SetCurrentThreadId(u32 id) noexcept { tls_current_thread_id = id; }

// ---------------------------------------------------------------------------------------
// ObjectTable
// ---------------------------------------------------------------------------------------

u32 ObjectTable::Add(u32 base, std::shared_ptr<Lv2Object> obj) {
    std::lock_guard lock(mutex_);
    u32& next = next_[base];
    u32 id;
    do {
        id = base + (++next & 0x00FFFFFFu);  // IDs are never reused within a run (24-bit space)
    } while (objects_.count(id));
    obj->id = id;
    objects_.emplace(id, std::move(obj));
    return id;
}

bool ObjectTable::Remove(u32 id) {
    std::lock_guard lock(mutex_);
    return objects_.erase(id) != 0;
}

std::vector<std::shared_ptr<Lv2Object>> ObjectTable::Snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<Lv2Object>> out;
    out.reserve(objects_.size());
    for (const auto& [id, obj] : objects_) out.push_back(obj);
    return out;
}

// ---------------------------------------------------------------------------------------
// Kernel
// ---------------------------------------------------------------------------------------

Kernel::Kernel(vm::GuestMemory& memory, hle::HleRegistry& hle, SyscallTable& syscalls, ppu::PpuCodeCache& code,
               ProcessState& process)
    : memory_(memory),
      hle_(hle),
      syscalls_(syscalls),
      code_(code),
      process_(process),
      threads_(std::make_unique<ThreadManager>(*this)),
      spu_(std::make_unique<SpuManager>(*this)),
      mmapper_(std::make_unique<MmapperService>(*this)),
      prx_(std::make_unique<PrxManager>(*this)),
      fs_(std::make_unique<FsService>(*this)),
      timers_(std::make_unique<TimerService>(*this)) {}

Kernel::~Kernel() { Shutdown(); }

void Kernel::Shutdown() {
    // PPU first: it raises `terminating` (if nobody did yet) and joins the PPU host threads;
    // then every SPU group is stopped and its host threads joined.
    threads_->Shutdown();
    spu_->Shutdown();
    timers_->Shutdown();
}

void Kernel::RequestTermination(const ProcessEnd& cause) {
    {
        std::lock_guard lock(end_mutex_);
        if (!end_.ended) {
            end_ = cause;
            end_.ended = true;
        }
    }
    // Raise the flag before waking: every wait predicate reads it under the object lock.
    terminating_.store(true, std::memory_order_release);
    for (const auto& obj : objects_.Snapshot()) obj->WakeForTermination();
    threads_->WakeForTermination();
    mmapper_->WakeForTermination();  // threads suspended on a page fault
    timers_->WakeForTermination();
    {
        std::lock_guard lock(sleep_mutex_);
        sleep_cv_.notify_all();
    }
}

ProcessEnd Kernel::process_end() const {
    std::lock_guard lock(end_mutex_);
    return end_;
}

bool Kernel::Sleep(u64 usec) {
    if (!usec) {
        std::this_thread::yield();
        return !is_terminating();
    }
    std::unique_lock lock(sleep_mutex_);
    return Wait(lock, sleep_cv_, usec, [] { return false; }) == WaitResult::TimedOut;
}

void Kernel::RegisterSyscalls() {
    threads_->RegisterSyscalls(syscalls_);
    RegisterSyncSyscalls(*this);
    RegisterEventSyscalls(*this);
    spu_->RegisterSyscalls(syscalls_);
    mmapper_->RegisterSyscalls(syscalls_);
    prx_->RegisterSyscalls(syscalls_);
    fs_->RegisterSyscalls(syscalls_);
    timers_->RegisterSyscalls(syscalls_);

    using C = ppu::PpuThreadContext;
    // 141 sys_timer_usleep(u64 usec), 142 sys_timer_sleep(u32 sec)
    syscalls_.Register(
        141, "sys_timer_usleep",
        [](void* self, C& ctx) -> s32 {
            static_cast<Kernel*>(self)->Sleep(ctx.gpr[3]);
            return CELL_OK;
        },
        this);
    syscalls_.Register(
        142, "sys_timer_sleep",
        [](void* self, C& ctx) -> s32 {
            static_cast<Kernel*>(self)->Sleep(static_cast<u64>(static_cast<u32>(ctx.gpr[3])) * 1000000);
            return CELL_OK;
        },
        this);
}

}  // namespace cell::lv2
