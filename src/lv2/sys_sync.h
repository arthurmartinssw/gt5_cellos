// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_sync.h
//
// lv2 synchronization objects. Syscalls (lv2 numbering):
//   82 sys_event_flag_create  83 _destroy  85 _wait  86 _trywait  87 _set
//   118 _clear  132 _cancel  139 _get
//   90 sys_semaphore_create   91 _destroy  92 _wait  93 _trywait  94 _post  114 _get_value
//   100 sys_mutex_create      101 _destroy 102 _lock 103 _trylock 104 _unlock
//   105 sys_cond_create       106 _destroy 107 _wait 108 _signal  109 _signal_all 110 _signal_to
//
// sysPrxForUser's lwmutex/lwcond (HLE) reuse Lv2Mutex/Lv2Cond: same semantics, plus the
// lwmutex variant mirrors owner/waiters/recursion into the guest sys_lwmutex_t so code that
// peeks at it (liblv2 inlines, debuggers) sees consistent values.
//
// Each object has one std::mutex `m`; a condition variable's waiters sleep on its mutex's `m`,
// so "release the mutex and wait" is atomic exactly as lv2 requires. Wake-ups use notify_all
// because waits can time out: a notify_one consumed by a timing-out waiter would strand the
// others. Priority protocols are accepted and recorded; wake order is the host's.
#pragma once

#include <array>
#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lv2/kernel.h"

namespace cell::lv2 {

inline constexpr u32 SYS_EVENT_FLAG_WAIT_AND = 0x01;
inline constexpr u32 SYS_EVENT_FLAG_WAIT_OR = 0x02;
inline constexpr u32 SYS_EVENT_FLAG_WAIT_CLEAR = 0x10;
inline constexpr u32 SYS_EVENT_FLAG_WAIT_CLEAR_ALL = 0x20;
inline constexpr s32 SYS_SYNC_WAITER_SINGLE = 0x10000;
inline constexpr s32 SYS_SYNC_WAITER_MULTIPLE = 0x20000;
inline constexpr u32 kLwmutexFree = 0xFFFFFFFFu;

struct Lv2Mutex : Lv2Object {
    std::mutex m;
    std::condition_variable cv;
    u32 protocol = SYS_SYNC_PRIORITY;
    bool recursive = false;
    u32 owner = 0;       // thread id, 0 = free
    u32 lock_count = 0;  // extra recursive acquisitions beyond the first
    u32 waiters = 0;
    u32 cond_refs = 0;   // sys_cond objects bound to this mutex
    bool destroyed = false;
    // lwmutex only: guest sys_lwmutex_t to keep in sync (0 = plain sys_mutex).
    vm::GuestMemory* mirror_memory = nullptr;
    u32 mirror_addr = 0;

    void WakeForTermination() override;
    std::string Describe() override;
    s32 Lock(Kernel& kernel, u32 tid, u64 timeout_us);
    s32 TryLock(u32 tid);
    s32 Unlock(u32 tid);
    void MirrorLocked();  // m held
};

struct Lv2Cond : Lv2Object {
    std::shared_ptr<Lv2Mutex> mutex;
    std::condition_variable cv;  // waits on mutex->m
    struct Waiter {
        u32 tid;
        bool signaled;
    };
    std::list<Waiter*> waiters;  // FIFO
    bool destroyed = false;
    // Diagnostics (guarded by mutex->m): who signals, and whether signals find a waiter.
    u64 waits = 0;
    u64 signals = 0;            // signal + signal_to + signal_all calls
    u64 unmatched_signals = 0;  // signals that found no waiter (not an error: condvar semantics)
    u64 last_signal_us = 0;
    u32 last_signaler = 0;
    // Provenance (set once by the creating call, before the object is published).
    u32 creator = 0;      // thread id of the creating call (0 = host / test)
    u32 creator_lr = 0;   // guest LR of that call
    u64 created_us = 0;   // system time
    // The first distinct threads that ever signaled / waited (guarded by mutex->m): who is
    // *expected* to produce for this condition when it is stuck — a wait-for chain for the
    // stall report. 0 = free slot.
    static constexpr std::size_t kHistory = 4;
    std::array<u32, kHistory> signaler_history{};
    std::array<u32, kHistory> waiter_history{};
    void SetCreator(u32 tid, u32 lr);
    // One line with the counters above, also when nobody waits (stall-report decoding).
    std::string Stats();
    // Copy of signaler_history (try_lock; empty if busy).
    [[nodiscard]] std::vector<u32> Signalers();

    void WakeForTermination() override;
    std::string Describe() override;
    s32 Wait(Kernel& kernel, u32 tid, u64 timeout_us);
    s32 Signal(u32 signaler = 0);
    s32 SignalAll(u32 signaler = 0);
    s32 SignalTo(u32 tid, u32 signaler = 0);
};

struct Lv2Semaphore : Lv2Object {
    std::mutex m;
    std::condition_variable cv;
    u32 protocol = SYS_SYNC_PRIORITY;
    s32 count = 0;
    s32 max = 1;
    u32 waiters = 0;
    bool destroyed = false;

    void WakeForTermination() override;
    std::string Describe() override;
    s32 Wait(Kernel& kernel, u64 timeout_us);
    s32 TryWait();
    s32 Post(s32 n);
};

struct Lv2EventFlag : Lv2Object {
    std::mutex m;
    std::condition_variable cv;
    u32 protocol = SYS_SYNC_PRIORITY;
    s32 type = SYS_SYNC_WAITER_SINGLE;
    u64 pattern = 0;
    u32 waiters = 0;
    u64 cancel_generation = 0;
    bool destroyed = false;

    void WakeForTermination() override;
    std::string Describe() override;
    s32 Wait(Kernel& kernel, u64 bits, u32 mode, u64* result, u64 timeout_us);
    s32 TryWait(u64 bits, u32 mode, u64* result);
    void Set(u64 bits);
    void Clear(u64 bits);
    u32 Cancel();
};

// lv2 attribute names: 8 bytes, NUL-terminated only if shorter.
[[nodiscard]] std::string ReadObjectName(const vm::GuestMemory& memory, u32 addr);

void RegisterSyncSyscalls(Kernel& kernel);

}  // namespace cell::lv2
