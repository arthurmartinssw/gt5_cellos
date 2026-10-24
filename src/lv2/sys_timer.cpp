// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_timer.cpp
#include "lv2/sys_timer.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>

#include "common/host_thread.h"
#include "common/log.h"
#include "lv2/sys_event.h"

namespace cell::lv2 {

namespace {
u64 Now() { return ppu::PpuInterpreter::SystemTimeMicroseconds(); }
u64 AddSat(u64 a, u64 b) { return a > ~u64{0} - b ? ~u64{0} : a + b; }
}  // namespace

std::string Lv2Timer::Describe() {
    char line[224];
    std::unique_lock lock(m, std::try_to_lock);
    if (!lock) {
        std::snprintf(line, sizeof(line), "timer 0x%08x: busy", id);
        return line;
    }
    const u64 now = Now();
    char last[48] = "never";
    if (fired) std::snprintf(last, sizeof(last), "%.1f ms ago", static_cast<double>(now - std::min(now, last_fire_us)) / 1000.0);
    std::snprintf(line, sizeof(line), "timer 0x%08x: %s, period %" PRIu64 " us, %s queue 0x%08x, %" PRIu64 " event(s) sent (last %s), %" PRIu64
                  " dropped",
                  id, state == SYS_TIMER_STATE_RUN ? "running" : "stopped", period, connected ? "connected to" : "not connected,", queue_id,
                  fired, last, dropped);
    return line;
}

TimerService::~TimerService() { Shutdown(); }

void TimerService::Shutdown() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void TimerService::WakeForTermination() { Kick(); }

void TimerService::Kick() {
    {
        std::lock_guard lock(mutex_);
        kicked_ = true;
    }
    cv_.notify_all();
}

void TimerService::EnsureThread() {
    std::lock_guard lock(mutex_);
    if (!realtime_ || stop_ || thread_.joinable()) return;
    thread_ = std::thread(&TimerService::Main, this);
}

// ---------------------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------------------

s32 TimerService::Create(u32* id) {
    auto t = std::make_shared<Lv2Timer>();
    *id = kernel_.objects().Add(kIdTimer, t);
    {
        std::lock_guard lock(mutex_);
        std::erase_if(timers_, [](const auto& w) { return w.expired(); });
        timers_.push_back(t);
    }
    LOG_INFO("lv2", "sys_timer_create -> 0x%08x", *id);
    return CELL_OK;
}

s32 TimerService::Destroy(u32 id) {
    const auto t = kernel_.objects().Get<Lv2Timer>(id);
    if (!t) return CELL_ESRCH;
    {
        std::lock_guard lock(t->m);
        if (t->connected) return CELL_EISCONN;
        t->state = SYS_TIMER_STATE_STOP;
    }
    kernel_.objects().Remove(id);
    return CELL_OK;
}

s32 TimerService::GetInformation(u32 id, u64* next_expire, u64* period, u32* state) {
    const auto t = kernel_.objects().Get<Lv2Timer>(id);
    if (!t) return CELL_ESRCH;
    Poll(Now());  // deliver a due expiry first, as RPCS3 does
    std::lock_guard lock(t->m);
    *next_expire = t->expire;
    *period = t->period;
    *state = t->state;
    return CELL_OK;
}

s32 TimerService::Start(u32 id, u64 base_time, u64 period) {
    const u64 now = Now();
    if (period && period < 100) return CELL_EINVAL;
    const auto t = kernel_.objects().Get<Lv2Timer>(id);
    if (!t) return CELL_ESRCH;
    {
        std::lock_guard lock(t->m);
        if (!t->connected) return CELL_ENOTCONN;
        if (t->state != SYS_TIMER_STATE_STOP) return CELL_EBUSY;
        u64 expire;
        if (period == 0) {
            if (base_time <= now) return CELL_ETIMEDOUT;  // one-shot in the past: not started
            expire = base_time;
        } else if (base_time == 0) {
            expire = AddSat(now, period);
        } else if (now < AddSat(base_time, period)) {
            expire = AddSat(base_time, period);
        } else {
            // First point of the grid {base_time + k * period} that is not before now.
            const u64 aligned = AddSat(now - now % period, base_time % period);
            expire = AddSat(aligned, aligned < now ? period : 0);
        }
        t->expire = expire;
        t->period = period;
        t->state = SYS_TIMER_STATE_RUN;
        LOG_INFO("lv2", "_sys_timer_start(0x%08x, base %" PRIu64 ", period %" PRIu64 " us): first expiry in %" PRIu64 " us -> queue 0x%08x",
                 id, base_time, period, expire - now, t->queue_id);
    }
    EnsureThread();
    Kick();
    return CELL_OK;
}

s32 TimerService::Stop(u32 id) {
    const auto t = kernel_.objects().Get<Lv2Timer>(id);
    if (!t) return CELL_ESRCH;
    Poll(Now());
    std::lock_guard lock(t->m);
    t->state = SYS_TIMER_STATE_STOP;
    return CELL_OK;
}

s32 TimerService::Connect(u32 id, u32 queue_id, u64 name, u64 data1, u64 data2) {
    const auto t = kernel_.objects().Get<Lv2Timer>(id);
    const auto q = kernel_.objects().Get<Lv2EventQueue>(queue_id);
    if (!t || !q) return CELL_ESRCH;
    std::lock_guard lock(t->m);
    if (t->connected) return CELL_EISCONN;
    t->connected = true;
    t->queue = q;
    t->queue_id = queue_id;
    t->source = name ? name : (u64{kProcessId} << 32) | id;
    t->data1 = data1;
    t->data2 = data2;
    LOG_INFO("lv2", "sys_timer_connect_event_queue(0x%08x -> queue 0x%08x, source 0x%016" PRIx64 ")", id, queue_id, t->source);
    return CELL_OK;
}

s32 TimerService::Disconnect(u32 id) {
    const auto t = kernel_.objects().Get<Lv2Timer>(id);
    if (!t) return CELL_ESRCH;
    Poll(Now());
    std::lock_guard lock(t->m);
    t->state = SYS_TIMER_STATE_STOP;
    if (!t->connected) return CELL_ENOTCONN;
    t->connected = false;
    t->queue.reset();
    t->queue_id = 0;
    return CELL_OK;
}

// ---------------------------------------------------------------------------------------
// Expiry
// ---------------------------------------------------------------------------------------

u64 TimerService::Poll(u64 now) {
    std::vector<std::shared_ptr<Lv2Timer>> list;
    {
        std::lock_guard lock(mutex_);
        for (const auto& w : timers_)
            if (auto t = w.lock()) list.push_back(std::move(t));
    }
    u64 earliest = ~u64{0};
    for (const auto& t : list) {
        std::lock_guard lock(t->m);  // timer lock, then the queue lock inside Push: never the reverse
        if (t->state != SYS_TIMER_STATE_RUN) continue;
        if (now >= t->expire) {
            const u64 shot = t->expire;
            const auto q = t->queue.lock();
            if (q && q->Push({t->source, t->data1, t->data2, shot}) == CELL_OK) {
                ++t->fired;
                t->last_fire_us = now;
            } else {
                ++t->dropped;
            }
            if (t->period) {
                u64 next = AddSat(shot, t->period);
                if (next + 100 * t->period < now) next = AddSat(now - (now - shot) % t->period, t->period);  // far behind: skip
                t->expire = next;
            } else {
                t->state = SYS_TIMER_STATE_STOP;
            }
        }
        if (t->state == SYS_TIMER_STATE_RUN) earliest = std::min(earliest, t->expire);
    }
    return earliest;
}

void TimerService::Main() {
    host::SetCurrentThreadName("lv2 timers");
    using Clock = std::chrono::steady_clock;
    for (;;) {
        const u64 now = Now();
        const u64 next = Poll(now);
        std::unique_lock lock(mutex_);
        if (stop_ || kernel_.is_terminating()) break;
        if (kicked_) {
            kicked_ = false;
            continue;
        }
        const auto wait = next == ~u64{0} ? std::chrono::microseconds(100'000)
                                          : std::chrono::microseconds(next > now ? std::min<u64>(next - now, 100'000) : 0);
        cv_.wait_until(lock, Clock::now() + wait, [&] { return stop_ || kicked_; });
        kicked_ = false;
        if (stop_) break;
    }
}

// ---------------------------------------------------------------------------------------
// Syscalls 70..76
// ---------------------------------------------------------------------------------------

void TimerService::RegisterSyscalls(SyscallTable& table) {
    using C = ppu::PpuThreadContext;
    static constexpr auto S = [](void* p) -> TimerService& { return *static_cast<TimerService*>(p); };
    static constexpr auto A32 = [](const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); };

    table.Register(70, "sys_timer_create", [](void* p, C& c) -> s32 {
        TimerService& s = S(p);
        const u32 out = A32(c, 0);
        if (!out || !s.kernel_.memory().Check(out, 4, vm::kPageWritable)) return CELL_EFAULT;
        u32 id = 0;
        const s32 r = s.Create(&id);
        if (r == CELL_OK) s.kernel_.memory().Write<u32>(out, id);
        return r;
    }, this);
    table.Register(71, "sys_timer_destroy", [](void* p, C& c) -> s32 { return S(p).Destroy(A32(c, 0)); }, this);
    table.Register(72, "sys_timer_get_information", [](void* p, C& c) -> s32 {
        TimerService& s = S(p);
        const u32 info = A32(c, 1);
        if (!info || !s.kernel_.memory().Check(info, 24, vm::kPageWritable)) return CELL_EFAULT;
        u64 next = 0, period = 0;
        u32 state = 0;
        const s32 r = s.GetInformation(A32(c, 0), &next, &period, &state);
        if (r == CELL_OK) {
            vm::GuestMemory& m = s.kernel_.memory();
            m.Write<u64>(info + 0, next);
            m.Write<u64>(info + 8, period);
            m.Write<u32>(info + 16, state);
            m.Write<u32>(info + 20, 0);
        }
        return r;
    }, this);
    table.Register(73, "_sys_timer_start", [](void* p, C& c) -> s32 { return S(p).Start(A32(c, 0), c.gpr[4], c.gpr[5]); }, this);
    table.Register(74, "sys_timer_stop", [](void* p, C& c) -> s32 { return S(p).Stop(A32(c, 0)); }, this);
    table.Register(75, "sys_timer_connect_event_queue", [](void* p, C& c) -> s32 {
        return S(p).Connect(A32(c, 0), A32(c, 1), c.gpr[5], c.gpr[6], c.gpr[7]);
    }, this);
    table.Register(76, "sys_timer_disconnect_event_queue", [](void* p, C& c) -> s32 { return S(p).Disconnect(A32(c, 0)); }, this);
}

}  // namespace cell::lv2
