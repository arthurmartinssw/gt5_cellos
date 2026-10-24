// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_timer.h
//
// lv2 event timers. A timer is an lv2 object that, once connected to an event queue and
// started, sends an event to that queue at its expiry (one-shot) or every `period` (periodic):
// threads blocked in sys_event_queue_receive on that queue are woken by it. Numbering verified
// against RPCS3's syscall table (Emu/Cell/lv2/lv2.cpp, 70..76); semantics follow RPCS3's
// sys_timer.cpp:
//   70 sys_timer_create(u32* timer_id)
//   71 sys_timer_destroy(u32 timer_id)                       EISCONN while connected
//   72 sys_timer_get_information(u32 timer_id, sys_timer_information_t* info)
//        info: be64 next_expire (system time, us), be64 period, be32 timer_state, be32 pad
//   73 _sys_timer_start(u32 timer_id, u64 base_time, u64 period)
//        EINVAL period != 0 && period < 100; ESRCH; ENOTCONN not connected; EBUSY running;
//        one-shot (period 0): expires at base_time, ETIMEDOUT if base_time is not in the future;
//        periodic: base_time 0 -> now + period; otherwise the first expiry on the period grid
//        anchored at base_time that is after now.
//   74 sys_timer_stop(u32 timer_id)
//   75 sys_timer_connect_event_queue(u32 timer_id, u32 queue_id, u64 name, u64 data1, u64 data2)
//        ESRCH timer/queue; EISCONN already connected. Event source = name, or
//        (pid << 32) | timer_id when name is 0.
//   76 sys_timer_disconnect_event_queue(u32 timer_id)        stops the timer; ENOTCONN
// Event: {source, data1, data2, expiry time of this shot (system time, us)}.
// System time is sys_time_get_system_time's clock (PpuInterpreter::SystemTimeMicroseconds).
//
// PC mapping: one host thread for all timers, sleeping until the earliest expiry (woken early
// by start/stop/connect). A periodic timer that falls far behind (debugger pause) skips ahead
// instead of bursting hundreds of catch-up events.
#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lv2/kernel.h"

namespace cell::lv2 {

struct Lv2EventQueue;

inline constexpr u32 kIdTimer = 0x11000000;
inline constexpr u32 SYS_TIMER_STATE_STOP = 0;
inline constexpr u32 SYS_TIMER_STATE_RUN = 1;

struct Lv2Timer : Lv2Object {
    std::mutex m;
    // ---- guarded by m ----
    u32 state = SYS_TIMER_STATE_STOP;
    u64 expire = 0;  // next expiry, system time (us)
    u64 period = 0;  // 0 = one-shot
    bool connected = false;
    std::weak_ptr<Lv2EventQueue> queue;
    u32 queue_id = 0;
    u64 source = 0, data1 = 0, data2 = 0;
    u64 fired = 0;    // events delivered
    u64 dropped = 0;  // queue full / destroyed at expiry
    u64 last_fire_us = 0;

    std::string Describe() override;
};

class TimerService {
public:
    explicit TimerService(Kernel& kernel) noexcept : kernel_(kernel) {}
    ~TimerService();
    TimerService(const TimerService&) = delete;
    TimerService& operator=(const TimerService&) = delete;

    s32 Create(u32* id);
    s32 Destroy(u32 id);
    s32 GetInformation(u32 id, u64* next_expire, u64* period, u32* state);
    s32 Start(u32 id, u64 base_time, u64 period);
    s32 Stop(u32 id);
    s32 Connect(u32 id, u32 queue_id, u64 name, u64 data1, u64 data2);
    s32 Disconnect(u32 id);

    // Delivers every expiry due at `now_us`; returns the earliest pending expiry (~0 if none).
    // Called by the timer thread; tests call it directly with a chosen time.
    u64 Poll(u64 now_us);
    // realtime = false: no host thread (tests drive Poll()).
    void set_realtime(bool on) noexcept { realtime_ = on; }

    void RegisterSyscalls(SyscallTable& table);
    void Shutdown();  // joins the timer thread; idempotent
    void WakeForTermination();

private:
    void EnsureThread();
    void Main();
    void Kick();

    Kernel& kernel_;
    bool realtime_ = true;
    std::mutex mutex_;  // timers_, thread state; never held while taking a timer or queue lock
    std::condition_variable cv_;
    std::vector<std::weak_ptr<Lv2Timer>> timers_;
    std::thread thread_;
    bool stop_ = false;
    bool kicked_ = false;
};

}  // namespace cell::lv2
