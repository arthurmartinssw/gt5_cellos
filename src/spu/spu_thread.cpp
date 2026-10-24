// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — spu/spu_thread.cpp
//
// Host side of an SPU thread. Threading rules:
//   * The executor (SPU interpreter/JIT, next phase) runs on the thread's own host thread and
//     is the only code touching ctx and mfc while the group runs.
//   * Everything the PPU can also reach (mailboxes, signals, events, ports, exit state) is
//     guarded by `m`; blocking channel operations sleep on `cv` through Kernel::Wait, so
//     process termination always wakes them. A group stop (terminate / group exit) sets
//     stop_requested and calls WakeWaiters(), which also wakes a wait on a bound SPU queue.
//   * `m` is never held while taking another object's mutex (event queue, event flag, group).
#include "spu/spu_thread.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "common/host_thread.h"
#include "common/log.h"
#include "lv2/sys_event.h"
#include "lv2/sys_spu.h"
#include "lv2/sys_sync.h"
#include "ppu/ppu_interpreter.h"
#include "spu/spu_mfc.h"

namespace cell::spu {

namespace {

constexpr u32 kInitialStackPointer = 0x3FFF0;  // lv2 starts SPU threads with r1 = top of LS - 16

void SetPreferredWord(ppu::v128& reg, u32 value) {
    std::memset(reg.bytes, 0, sizeof(reg.bytes));
    for (int i = 0; i < 4; ++i) reg.bytes[i] = static_cast<u8>(value >> (24 - 8 * i));
}

void SetPreferredDoubleword(ppu::v128& reg, u64 value) {
    std::memset(reg.bytes, 0, sizeof(reg.bytes));
    for (int i = 0; i < 8; ++i) reg.bytes[i] = static_cast<u8>(value >> (56 - 8 * i));
}

u64 MicrosecondsFromTicks(u64 ticks) {
    return ticks * 1'000'000 / ppu::PpuInterpreter::kTimebaseFrequency;
}

}  // namespace

// =======================================================================================
// Helpers
// =======================================================================================

namespace {
// Marks the thread as inside a channel/stop service for as long as the call lasts.
struct LiveWaitScope {
    SpuThread& t;
    LiveWaitScope(SpuThread& thread, s32 what) noexcept : t(thread) {
        t.live_pc.store(t.ctx.pc, std::memory_order_relaxed);
        t.live_wait.store(what, std::memory_order_relaxed);
    }
    ~LiveWaitScope() { t.live_wait.store(-1, std::memory_order_relaxed); }
    LiveWaitScope(const LiveWaitScope&) = delete;
    LiveWaitScope& operator=(const LiveWaitScope&) = delete;
};
}  // namespace

bool SpuThread::ShouldStop() const noexcept {
    return stop_requested.load(std::memory_order_acquire) || kernel.is_terminating();
}

template <typename Pred>
bool SpuThread::BlockUntil(std::unique_lock<std::mutex>& lock, Pred ready) {
    const auto r = kernel.Wait(lock, cv, 0, [&] { return ready() || stop_requested.load(std::memory_order_acquire); });
    return r == lv2::WaitResult::Ready && !stop_requested.load(std::memory_order_acquire);
}

bool SpuThread::FailLocked(std::unique_lock<std::mutex>& lock, std::string message) {
    lock.unlock();
    Fault(std::move(message));
    lock.lock();
    return false;
}

u32 SpuThread::DecrementerLocked() const {
    return dec_value - static_cast<u32>(ppu::PpuInterpreter::TimeBase() - dec_start_tb);
}

// Latches the level-type event conditions into event_stat and returns the unmasked pending set.
// Edge-type events (LE, ME, TG, SN) are latched where they happen.
u32 SpuThread::PendingEventsLocked() {
    if (!in_mbox.empty()) event_stat |= SPU_EVENT_MB;
    if (snr_valid[0]) event_stat |= SPU_EVENT_S1;
    if (snr_valid[1]) event_stat |= SPU_EVENT_S2;
    if (!dec_event_fired && ppu::PpuInterpreter::TimeBase() - dec_start_tb > dec_value) {
        dec_event_fired = true;  // decrementer went past zero
        event_stat |= SPU_EVENT_TM;
    }
    if ((event_mask & SPU_EVENT_LR) && MfcReservationLost(*this)) event_stat |= SPU_EVENT_LR;
    return event_stat & event_mask;
}

// =======================================================================================
// Channels
// =======================================================================================

bool SpuThread::ReadChannel(u32 channel, u32& value) {
    LiveWaitScope live(*this, static_cast<s32>(channel));
    std::unique_lock lock(m);
    switch (channel) {
    case SPU_RdEventStat:
        for (;;) {
            if ((value = PendingEventsLocked()) != 0) return true;
            if (event_mask == 0) return FailLocked(lock, "SPU_RdEventStat read with an empty event mask (would wait forever)");
            // MB/S1/S2 arrive with a notify; TM and LR are polled: wake when the decrementer
            // expires, or every 20 us while a reservation is being watched.
            u64 timeout_us = 0;
            if (event_mask & SPU_EVENT_LR) timeout_us = 20;
            if ((event_mask & SPU_EVENT_TM) && !dec_event_fired) {
                const u64 elapsed = ppu::PpuInterpreter::TimeBase() - dec_start_tb;
                const u64 left = elapsed >= dec_value ? 1 : std::max<u64>(1, MicrosecondsFromTicks(dec_value - elapsed));
                timeout_us = timeout_us ? std::min(timeout_us, left) : left;
            }
            const auto r = kernel.Wait(lock, cv, timeout_us, [&] {
                return stop_requested.load(std::memory_order_acquire) || (PendingEventsLocked() != 0);
            });
            if (r == lv2::WaitResult::Terminated || stop_requested.load(std::memory_order_acquire)) return false;
        }
    case SPU_RdSigNotify1:
    case SPU_RdSigNotify2: {
        const u32 n = channel - SPU_RdSigNotify1;
        if (!BlockUntil(lock, [&] { return snr_valid[n]; })) return false;
        value = snr[n];
        snr[n] = 0;
        snr_valid[n] = false;
        return true;
    }
    case SPU_RdDec:
        value = DecrementerLocked();
        return true;
    case SPU_RdEventMask:
        value = event_mask;
        return true;
    case MFC_RdTagMask:
        value = mfc.tag_mask;
        return true;
    case SPU_RdMachStat:
        value = 0;  // not isolated, interrupts disabled
        return true;
    case SPU_RdSRR0:
        value = ctx.srr0;
        return true;
    case MFC_RdTagStat:
        value = mfc.tag_mask;  // every command completed when it was issued
        return true;
    case MFC_RdListStallStat:
        if (!mfc.stall_mask) return FailLocked(lock, "MFC_RdListStallStat read with no stalled list (would wait forever)");
        value = mfc.stall_mask;
        mfc.stall_mask = 0;
        return true;
    case MFC_RdAtomicStat:
        if (!mfc.atomic_pending) return FailLocked(lock, "MFC_RdAtomicStat read with no atomic command issued (would wait forever)");
        value = mfc.atomic_stat;
        mfc.atomic_pending = false;
        return true;
    case SPU_RdInMbox:
        if (!BlockUntil(lock, [&] { return !in_mbox.empty(); })) return false;
        value = in_mbox.front();
        in_mbox.pop_front();
        cv.notify_all();
        return true;
    default: {
        char text[64];
        std::snprintf(text, sizeof(text), "rdch on invalid channel %u", channel);
        return FailLocked(lock, text);
    }
    }
}

bool SpuThread::WriteChannel(u32 channel, u32 value) {
    LiveWaitScope live(*this, static_cast<s32>(channel));
    // MFC parameter channels belong to the executor alone: no lock.
    switch (channel) {
    case MFC_LSA: mfc.lsa = value; return true;
    case MFC_EAH: mfc.eah = value; return true;
    case MFC_EAL: mfc.eal = value; return true;
    case MFC_Size: mfc.size = value & 0xFFFF; return true;
    case MFC_TagID: mfc.tag = value & 0x1F; return true;
    case MFC_Cmd: return MfcExecute(*this, value);
    case MFC_WrTagMask: mfc.tag_mask = value; return true;
    case MFC_WrListStallAck: mfc.stall_mask &= ~(1u << (value & 31)); return true;
    case MFC_WrMSSyncReq: std::atomic_thread_fence(std::memory_order_seq_cst); return true;
    case SPU_WrSRR0: ctx.srr0 = value & kLsMask & ~3u; return true;
    default: break;
    }

    std::unique_lock lock(m);
    switch (channel) {
    case SPU_WrEventMask:
        event_mask = value;
        return true;
    case SPU_WrEventAck:
        event_stat &= ~value;
        return true;
    case SPU_WrDec:
        dec_value = value;
        dec_start_tb = ppu::PpuInterpreter::TimeBase();
        dec_event_fired = false;
        return true;
    case MFC_WrTagUpdate:
        if (value != MFC_TAG_UPDATE_IMMEDIATE) event_stat |= SPU_EVENT_TG;  // already complete
        return true;
    case SPU_WrOutMbox:
        // Consumed by the stop codes (thread/group exit, receive event) and by the
        // out-interrupt-mailbox protocol; blocks while still full, as on hardware.
        if (!BlockUntil(lock, [&] { return !out_mbox.has_value(); })) return false;
        out_mbox = value;
        return true;
    case SPU_WrOutIntrMbox:
        return HandleOutIntrMailbox(lock, value);
    default: {
        char text[64];
        std::snprintf(text, sizeof(text), "wrch on invalid channel %u (value 0x%08x)", channel, value);
        return FailLocked(lock, text);
    }
    }
}

u32 SpuThread::ChannelCount(u32 channel) {
    std::unique_lock lock(m);
    switch (channel) {
    case SPU_RdEventStat: return PendingEventsLocked() ? 1 : 0;
    case SPU_RdSigNotify1: return snr_valid[0] ? 1 : 0;
    case SPU_RdSigNotify2: return snr_valid[1] ? 1 : 0;
    case MFC_Cmd: return 16;  // queue always empty
    case MFC_RdListStallStat: return mfc.stall_mask ? 1 : 0;
    case MFC_RdAtomicStat: return mfc.atomic_pending ? 1 : 0;
    case SPU_WrOutMbox: return out_mbox ? 0 : 1;
    case SPU_RdInMbox: return static_cast<u32>(in_mbox.size());
    case SPU_WrEventMask:
    case SPU_WrEventAck:
    case SPU_WrDec:
    case SPU_RdDec:
    case MFC_WrMSSyncReq:
    case SPU_RdEventMask:
    case MFC_RdTagMask:
    case SPU_RdMachStat:
    case SPU_WrSRR0:
    case SPU_RdSRR0:
    case MFC_LSA:
    case MFC_EAH:
    case MFC_EAL:
    case MFC_Size:
    case MFC_TagID:
    case MFC_WrTagMask:
    case MFC_WrTagUpdate:
    case MFC_RdTagStat:
    case MFC_WrListStallAck:
    case SPU_WrOutIntrMbox:
        return 1;
    default:
        LOG_WARN("spu", "rchcnt on invalid channel %u", channel);
        return 0;
    }
}

// =======================================================================================
// lv2 SPU thread services
// =======================================================================================

// SPU_WrOutIntrMbox carries lv2 requests (liblv2's spu_thread.h inlines):
//   code   0..63  sys_spu_thread_send_event(spup, data0, data1): out_mbox = data1, then
//                 (spup << 24 | data0); lv2 pushes {USER_KEY, thread id, spup << 32 | data0,
//                 data1} to the queue connected on spup and replies the result in in_mbox
//   code  64..127 sys_spu_thread_throw_event: same, no reply
//   code 128      sys_event_flag_set_bit(flag_id in out_mbox, bit): reply in in_mbox
//   code 192      sys_event_flag_set_bit_impatient: no reply
bool SpuThread::HandleOutIntrMailbox(std::unique_lock<std::mutex>& lock, u32 value) {
    const u32 code = value >> 24;
    char text[160];
    if (code >= 128 && code != 128 && code != 192) {
        std::snprintf(text, sizeof(text), "unknown SPU_WrOutIntrMbox request 0x%08x", value);
        return FailLocked(lock, text);
    }
    if (!out_mbox) {
        std::snprintf(text, sizeof(text), "SPU_WrOutIntrMbox request 0x%08x with an empty SPU_WrOutMbox", value);
        return FailLocked(lock, text);
    }
    const u32 data = *out_mbox;
    out_mbox.reset();
    event_stat |= SPU_EVENT_LE;
    cv.notify_all();
    const bool reply = code < 64 || code == 128;
    if (reply && !in_mbox.empty()) {
        // lv2 refuses while the reply slot is in use; the program sees CELL_EBUSY.
        in_mbox.assign(1, static_cast<u32>(CELL_EBUSY));
        event_stat |= SPU_EVENT_ME;
        cv.notify_all();
        return true;
    }

    s32 result = CELL_OK;
    lock.unlock();
    if (code < 128) {
        const u32 spup = code & 63;
        std::shared_ptr<lv2::Lv2EventQueue> queue;
        {
            std::lock_guard g(m);
            queue = ports[spup].lock();
        }
        result = queue ? queue->Push({SYS_SPU_THREAD_EVENT_USER_KEY, id, (u64{spup} << 32) | (value & 0x00FFFFFF), data})
                       : CELL_ENOTCONN;
        if (result != CELL_OK)
            LOG_TRACE("spu", "sys_spu_thread_%s_event(spup %u) -> 0x%08x", code < 64 ? "send" : "throw", spup,
                      static_cast<u32>(result));
    } else {
        const u32 bit = value & 0x00FFFFFF;
        if (bit > 63) {
            lock.lock();
            std::snprintf(text, sizeof(text), "sys_event_flag_set_bit: bit %u out of range", bit);
            return FailLocked(lock, text);
        }
        const auto flag = kernel.objects().Get<lv2::Lv2EventFlag>(data);
        if (flag) flag->Set(1ull << bit);
        else result = CELL_ESRCH;
    }
    lock.lock();
    if (reply) in_mbox.assign(1, static_cast<u32>(result));
    event_stat |= SPU_EVENT_ME;
    cv.notify_all();
    return true;
}

// stop 0x110 / 0x111: sys_spu_thread_receive_event / tryreceive_event. The SPU queue number
// comes in out_mbox; the reply is {result, data1, data2, data3} (low 32 bits) in in_mbox.
bool SpuThread::ReceiveEvent(std::unique_lock<std::mutex>& lock, bool try_only) {
    if (!out_mbox) return FailLocked(lock, "sys_spu_thread_receive_event: SPU_WrOutMbox (queue number) is empty");
    const u32 spuq = *out_mbox;
    out_mbox.reset();
    event_stat |= SPU_EVENT_LE;
    cv.notify_all();
    if (!in_mbox.empty()) {
        in_mbox.assign(1, static_cast<u32>(CELL_EBUSY));
        return true;
    }
    std::shared_ptr<lv2::Lv2EventQueue> queue;
    for (const auto& [number, weak] : bound_queues)
        if (number == spuq) queue = weak.lock();
    if (!queue) {
        in_mbox.assign(1, static_cast<u32>(CELL_EINVAL));
        return true;
    }

    lock.unlock();
    s32 result = CELL_OK;
    lv2::Lv2Event event{};
    {
        std::unique_lock q(queue->m);
        if (try_only) {
            if (queue->events.empty()) result = CELL_EBUSY;
        } else {
            ++queue->waiters;
            const auto r = kernel.Wait(q, queue->cv, 0, [&] {
                return !queue->events.empty() || queue->destroyed || stop_requested.load(std::memory_order_acquire);
            });
            --queue->waiters;
            if (r == lv2::WaitResult::Terminated || stop_requested.load(std::memory_order_acquire)) {
                q.unlock();
                lock.lock();
                return false;
            }
            if (queue->events.empty()) result = CELL_ECANCELED;  // destroyed while waiting
        }
        if (result == CELL_OK) {
            event = queue->events.front();
            queue->events.pop_front();
        }
    }
    lock.lock();
    if (result == CELL_OK)
        in_mbox.assign({static_cast<u32>(CELL_OK), static_cast<u32>(event.data1), static_cast<u32>(event.data2),
                        static_cast<u32>(event.data3)});
    else
        in_mbox.assign(1, static_cast<u32>(result));
    cv.notify_all();
    return true;
}

bool SpuThread::StopAndSignal(u32 code) {
    LiveWaitScope live(*this, kWaitStop | static_cast<s32>(code));
    std::unique_lock lock(m);
    switch (code) {
    case SYS_SPU_THREAD_STOP_YIELD:
        lock.unlock();
        std::this_thread::yield();
        return !ShouldStop();
    case SYS_SPU_THREAD_STOP_RECEIVE_EVENT:
        return ReceiveEvent(lock, false);
    case SYS_SPU_THREAD_STOP_TRY_RECEIVE_EVENT:
        return ReceiveEvent(lock, true);
    case SYS_SPU_THREAD_STOP_THREAD_EXIT:
    case SYS_SPU_THREAD_STOP_GROUP_EXIT: {
        const bool group_exit = code == SYS_SPU_THREAD_STOP_GROUP_EXIT;
        if (!out_mbox)
            return FailLocked(lock, group_exit ? "sys_spu_thread_group_exit: SPU_WrOutMbox (status) is empty"
                                               : "sys_spu_thread_exit: SPU_WrOutMbox (status) is empty");
        exit_status = *out_mbox;
        out_mbox.reset();
        exit = group_exit ? SpuExit::GroupExit : SpuExit::ThreadExit;
        const s32 status = static_cast<s32>(exit_status);
        lock.unlock();
        if (group.log_this_run.load(std::memory_order_relaxed))
            LOG_INFO("spu", "%s(%d)", group_exit ? "sys_spu_thread_group_exit" : "sys_spu_thread_exit", status);
        if (group_exit) group.RequestStop(SYS_SPU_THREAD_GROUP_JOIN_GROUP_EXIT, status);
        return false;
    }
    default: {
        char text[96];
        std::snprintf(text, sizeof(text), "stop-and-signal 0x%04x is not an lv2 service code", code);
        exit = SpuExit::StopCode;
        exit_status = code;
        return FailLocked(lock, text);
    }
    }
}

void SpuThread::Fault(std::string message) {
    {
        std::lock_guard lock(m);
        if (exit == SpuExit::None) exit = SpuExit::Fault;
        if (fault.empty()) fault = message;
    }
    char head[192];
    std::snprintf(head, sizeof(head), "SPU thread 0x%08x \"%s\" (group 0x%08x \"%s\", spu %u) at LS pc 0x%05x: ", id,
                  name.c_str(), group.id, group.name.c_str(), spu_num, ctx.pc);
    // SYS_SPU_THREAD_GROUP_EVENT_EXCEPTION to whoever connected a queue for it (SPURS installs
    // a handler). The process still ends with the report: SPU exceptions are fatal here.
    std::shared_ptr<lv2::Lv2EventQueue> exception_queue;
    {
        std::lock_guard lock(group.m);
        exception_queue = group.event_queues[1].lock();
    }
    if (exception_queue) exception_queue->Push({SYS_SPU_THREAD_GROUP_EVENT_EXCEPTION_KEY, group.id, id, 0});
    lv2::ProcessEnd end;
    end.kind = ppu::kExitAbort;
    end.thread_id = id;
    end.message = head + message;
    LOG_ERROR("spu", "%s", end.message.c_str());
    kernel.RequestTermination(end);
}

// =======================================================================================
// PPU-side operations
// =======================================================================================

void SpuThread::WriteInboundMailbox(u32 value) {
    std::lock_guard lock(m);
    if (in_mbox.size() >= kInMboxDepth) {
        // Hardware overwrites the newest entry when the PPU writes into a full mailbox.
        in_mbox.back() = value;
        LOG_WARN("spu", "inbound mailbox of SPU thread 0x%08x is full: last entry overwritten", id);
    } else {
        in_mbox.push_back(value);
    }
    cv.notify_all();
}

s32 SpuThread::WriteSignal(u32 number, u32 value) {
    if (number > 1) return CELL_EINVAL;
    std::lock_guard lock(m);
    if (snr_or_mode[number] && snr_valid[number]) snr[number] |= value;
    else snr[number] = value;
    snr_valid[number] = true;
    cv.notify_all();
    return CELL_OK;
}

bool SpuThread::ResetForStart(std::string* error) {
    if (!LoadSegments(kernel.memory(), image, ctx.ls, error)) return false;
    {
        std::lock_guard lock(m);
        in_mbox.clear();
        out_mbox.reset();
        snr[0] = snr[1] = 0;
        snr_valid[0] = snr_valid[1] = false;
        event_mask = event_stat = 0;
        dec_value = 0;
        dec_start_tb = ppu::PpuInterpreter::TimeBase();
        dec_event_fired = true;
        exit = SpuExit::None;
        exit_status = 0;
        fault.clear();
        for (int i = 0; i < 128; ++i) std::memset(ctx.gpr[i].bytes, 0, sizeof(ctx.gpr[i].bytes));
        ctx.pc = image.entry & kLsMask & ~3u;
        ctx.srr0 = 0;
        SetPreferredWord(ctx.gpr[1], kInitialStackPointer);
        for (int i = 0; i < 4; ++i) SetPreferredDoubleword(ctx.gpr[3 + i], args[i]);
    }
    mfc = MfcState{};
    stop_requested.store(false, std::memory_order_release);
    return true;
}

void SpuThread::WakeWaiters() {
    std::vector<std::shared_ptr<lv2::Lv2EventQueue>> queues;
    {
        std::lock_guard lock(m);
        cv.notify_all();
        for (const auto& [number, weak] : bound_queues)
            if (auto q = weak.lock()) queues.push_back(std::move(q));
    }
    for (const auto& q : queues) {
        std::lock_guard lock(q->m);
        q->cv.notify_all();
    }
}

void SpuThread::WakeForTermination() {
    std::lock_guard lock(m);
    cv.notify_all();
}

void SpuThread::HostMain() {
    char host_name[32];
    std::snprintf(host_name, sizeof(host_name), "SPU 0x%08x", id);
    host::SetCurrentThreadName(host_name);
    log::tls_thread_name = log_name.c_str();

    if (auto executor = kernel.spu().CreateExecutor(*this)) {
        try {
            executor->Run(*this);
        } catch (const std::exception& e) {
            Fault(std::string("SPU executor threw: ") + e.what());
        }
        last_run_insns = executor->executed();
    } else {
        char text[256];
        std::snprintf(text, sizeof(text),
                      "cannot run SPU code: no SPU executor is installed (the SPU ISA decoder / SPU->x86-64 "
                      "translator is the next phase). Image entry 0x%05x, %zu segments.",
                      image.entry, image.segments.size());
        Fault(text);
    }

    bool fell_through = false;
    {
        std::lock_guard lock(m);
        if (exit == SpuExit::None) {
            if (ShouldStop()) exit = SpuExit::Terminated;
            else fell_through = true;
        }
    }
    if (fell_through) Fault("SPU executor returned without stop, exit or fault");
    group.OnThreadStopped(*this);
    log::tls_thread_name = nullptr;
}

// =======================================================================================
// Thread group
// =======================================================================================

std::shared_ptr<SpuThread> SpuThreadGroup::Peer(u32 spu_num) {
    std::lock_guard lock(m);
    return spu_num < threads.size() ? threads[spu_num] : nullptr;
}

void SpuThreadGroup::OnThreadStopped(SpuThread& thread) {
    std::string summary;
    {
        std::lock_guard lock(m);
        run_insns += thread.last_run_insns;
        if (running) --running;
        if (running == 0) {
            if (join_cause == 0) join_cause = SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT;
            const u64 now = ppu::PpuInterpreter::SystemTimeMicroseconds();
            stats.OnStop(now, run_insns, join_cause, exit_status);
            const SpuRunLogConfig& cfg = RunLogConfig();
            if (!cfg.verbose.load(std::memory_order_relaxed) && stats.runs > cfg.first_runs.load(std::memory_order_relaxed) &&
                stats.WindowDue(now, cfg.summary_seconds.load(std::memory_order_relaxed)))
                summary = stats.TakeWindowSummary(now, id, name);
            state = State::Stopped;
            cv.notify_all();
        }
    }
    if (!summary.empty()) LOG_INFO("spu", "%s", summary.c_str());
}

void SpuThreadGroup::RequestStop(s32 cause, s32 status) {
    std::vector<std::shared_ptr<SpuThread>> list;
    {
        std::lock_guard lock(m);
        if (state != State::Running) return;
        if (join_cause == 0) {
            join_cause = cause;
            exit_status = status;
        }
        list = threads;
    }
    for (const auto& t : list) {
        if (!t) continue;
        t->stop_requested.store(true, std::memory_order_release);
        t->WakeWaiters();
    }
}

void SpuThreadGroup::JoinHosts() {
    std::vector<std::thread> list;
    {
        std::lock_guard lock(m);
        list.swap(hosts);
    }
    for (auto& h : list)
        if (h.joinable()) h.join();
}

void SpuThreadGroup::WakeForTermination() {
    std::lock_guard lock(m);
    cv.notify_all();
}

}  // namespace cell::spu
