// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/timer_pad_sync_tests.cpp
//
// Phase 7.6: what the GT5 primary-thread stall pointed at.
//   * lv2 timers (syscalls 70..76): argument/state errors in RPCS3's order, event source
//     (name or pid<<32|id), data1/data2, data3 = expiry of the shot, periodic re-arm, one-shot
//     stop, base-time grid alignment, real-time delivery to a blocked receiver, shutdown;
//   * cellPad (sys_io) with no controller: init/end checks, GetInfo/GetInfo2 layout, settings
//     stored, GetData / ClearBuf / GetCapabilityInfo -> NO_DEVICE with the buffer untouched;
//   * lv2 cond semantics under several waiters: signal wakes exactly one, signal_all the rest,
//     signal with no waiter is counted (not queued), timeout returns with the mutex re-held,
//     EPERM without the mutex; statistics for the stall report;
//   * stall report: a PPU thread in sys_lwcond_wait is decoded (2 arguments only, the lwcond and
//     lwmutex guest structs, the lv2 cond counters, the caller instruction); a thread in
//     sys_event_queue_receive lists the queue's producers (a connected timer, or none).
#include <atomic>
#include <chrono>
#include <thread>

#include "hle/cell_pad.h"
#include "hle/fnid.h"
#include "lv2/stall_monitor.h"
#include "lv2/sys_event.h"
#include "lv2/sys_sync.h"
#include "lv2/sys_timer.h"
#include "ppu_asm.h"
#include "test_machine.h"

using namespace cell;

namespace {

constexpr u32 kOut = kData + 0x100;

s32 Sys(Machine& m, u32 n, std::initializer_list<u64> args, ppu::PpuThreadContext* out = nullptr) {
    ppu::PpuThreadContext c{};
    c.gpr[11] = n;
    int r = 3;
    for (u64 a : args) c.gpr[r++] = a;
    m.syscalls.Dispatch(c);
    if (out) *out = c;
    return static_cast<s32>(c.gpr[3]);
}

s32 Call(Machine& m, const char* module, const char* name, std::initializer_list<u64> args, u32 tid = 0) {
    const u32 index = m.hle->Bind(module, hle::ComputeFnid(name));
    const hle::HleFunction& f = m.hle->At(index);
    ppu::PpuThreadContext c{};
    c.thread_id = tid;
    int r = 3;
    for (u64 a : args) c.gpr[r++] = a;
    f.fn(c, m.memory, f);
    return static_cast<s32>(c.gpr[3]);
}

u32 MakeQueue(Machine& m, u32 out = kData + 0x310) {
    m.memory.Write<u32>(kData + 0x300, lv2::SYS_SYNC_FIFO);
    m.memory.Write<u32>(kData + 0x304, lv2::SYS_PPU_QUEUE);
    m.memory.Write<u64>(kData + 0x308, 0);
    CHECK_EQ(Sys(m, 128, {out, kData + 0x300, 0, 32}), CELL_OK);
    return m.memory.Read<u32>(out);
}

// sys_event_queue_tryreceive: number of events taken into kData+0x800 (32 bytes each)
u32 TryReceive(Machine& m, u32 q) {
    CHECK_EQ(Sys(m, 131, {q, kData + 0x800, 8, kOut + 0x40}), CELL_OK);
    return m.memory.Read<u32>(kOut + 0x40);
}

u64 Now() { return ppu::PpuInterpreter::SystemTimeMicroseconds(); }

void TestTimerSyscalls() {
    std::printf("-- lv2 timers 70..76: errors, connect, periodic and one-shot expiry, info\n");
    Machine m;
    m.kernel->timers().set_realtime(false);  // expiries delivered by Poll() only
    CHECK_EQ(Sys(m, 70, {0}), CELL_EFAULT);
    CHECK_EQ(Sys(m, 70, {kOut}), CELL_OK);
    const u32 timer = m.memory.Read<u32>(kOut);
    CHECK_EQ(timer >> 24, lv2::kIdTimer >> 24);
    const u32 q = MakeQueue(m);

    CHECK_EQ(Sys(m, 73, {timer, 0, 50}), CELL_EINVAL);  // period below 100 us
    CHECK_EQ(Sys(m, 73, {0x11FFFFFF, 0, 1000}), CELL_ESRCH);
    CHECK_EQ(Sys(m, 73, {timer, 0, 1000}), CELL_ENOTCONN);
    CHECK_EQ(Sys(m, 75, {timer, 0x8DFFFFFF, 0, 1, 2}), CELL_ESRCH);
    CHECK_EQ(Sys(m, 75, {timer, q, 0, 0x11, 0x22}), CELL_OK);
    CHECK_EQ(Sys(m, 75, {timer, q, 0, 0x11, 0x22}), CELL_EISCONN);
    CHECK_EQ(Sys(m, 71, {timer}), CELL_EISCONN);  // destroy while connected

    // periodic, base 0: first expiry one period from now
    const u64 before = Now();
    CHECK_EQ(Sys(m, 73, {timer, 0, 1000}), CELL_OK);
    CHECK_EQ(Sys(m, 73, {timer, 0, 1000}), CELL_EBUSY);
    CHECK_EQ(Sys(m, 72, {timer, kOut}), CELL_OK);
    const u64 first = m.memory.Read<u64>(kOut);
    CHECK(first >= before + 1000 && first <= Now() + 1000);
    CHECK_EQ(m.memory.Read<u64>(kOut + 8), 1000);
    CHECK_EQ(m.memory.Read<u32>(kOut + 16), lv2::SYS_TIMER_STATE_RUN);
    CHECK_EQ(TryReceive(m, q), 0);
    CHECK_EQ(m.kernel->timers().Poll(first - 1), first);  // not yet
    CHECK_EQ(TryReceive(m, q), 0);
    CHECK_EQ(m.kernel->timers().Poll(first), first + 1000);
    CHECK_EQ(m.kernel->timers().Poll(first + 1000), first + 2000);
    CHECK_EQ(TryReceive(m, q), 2);
    const u32 ev = kData + 0x800;
    CHECK_EQ(m.memory.Read<u64>(ev + 0), (u64{lv2::kProcessId} << 32) | timer);  // source when name is 0
    CHECK_EQ(m.memory.Read<u64>(ev + 8), 0x11);
    CHECK_EQ(m.memory.Read<u64>(ev + 16), 0x22);
    CHECK_EQ(m.memory.Read<u64>(ev + 24), first);         // expiry of the first shot
    CHECK_EQ(m.memory.Read<u64>(ev + 32 + 24), first + 1000);

    CHECK_EQ(Sys(m, 74, {timer}), CELL_OK);
    CHECK_EQ(Sys(m, 72, {timer, kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut + 16), lv2::SYS_TIMER_STATE_STOP);
    CHECK_EQ(m.kernel->timers().Poll(first + 5000), ~u64{0});
    CHECK_EQ(TryReceive(m, q), 0);

    // one-shot: in the past -> ETIMEDOUT (not started); in the future -> one event, then stopped
    CHECK_EQ(Sys(m, 73, {timer, 1, 0}), CELL_ETIMEDOUT);
    const u64 at = Now() + 50'000;
    CHECK_EQ(Sys(m, 73, {timer, at, 0}), CELL_OK);
    CHECK_EQ(m.kernel->timers().Poll(at), ~u64{0});
    CHECK_EQ(TryReceive(m, q), 1);
    CHECK_EQ(m.memory.Read<u64>(ev + 24), at);
    CHECK_EQ(Sys(m, 72, {timer, kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut + 16), lv2::SYS_TIMER_STATE_STOP);

    // periodic with a base far in the past: first expiry on the base's grid, after now
    // (the guest clock starts near 0 when the process starts: make room for "10.5 ms ago")
    if (Now() < 20'000) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    const u64 now = Now();
    const u64 base = now - 10'500;
    CHECK_EQ(Sys(m, 73, {timer, base, 1000}), CELL_OK);
    CHECK_EQ(Sys(m, 72, {timer, kOut}), CELL_OK);
    const u64 grid = m.memory.Read<u64>(kOut);
    CHECK_EQ((grid - base) % 1000, 0);
    CHECK(grid >= now && grid < now + 2000);

    // a named source
    CHECK_EQ(Sys(m, 76, {timer}), CELL_OK);  // also stops it
    CHECK_EQ(Sys(m, 76, {timer}), CELL_ENOTCONN);
    CHECK_EQ(Sys(m, 75, {timer, q, 0xABCD, 0, 0}), CELL_OK);
    CHECK_EQ(Sys(m, 73, {timer, 0, 100}), CELL_OK);
    m.kernel->timers().Poll(Now() + 1000);
    CHECK_EQ(TryReceive(m, q), 1);
    CHECK_EQ(m.memory.Read<u64>(ev), 0xABCD);
    CHECK_EQ(Sys(m, 76, {timer}), CELL_OK);
    CHECK_EQ(Sys(m, 71, {timer}), CELL_OK);
    CHECK_EQ(Sys(m, 71, {timer}), CELL_ESRCH);
}

void TestTimerRealtime() {
    std::printf("-- lv2 timer thread: periodic events wake a blocked sys_event_queue_receive\n");
    Machine m;
    CHECK_EQ(Sys(m, 70, {kOut}), CELL_OK);
    const u32 timer = m.memory.Read<u32>(kOut);
    const u32 q = MakeQueue(m);
    CHECK_EQ(Sys(m, 75, {timer, q, 0x77, 0, 0}), CELL_OK);
    CHECK_EQ(Sys(m, 73, {timer, 0, 2000}), CELL_OK);  // 2 ms
    const auto start = std::chrono::steady_clock::now();
    ppu::PpuThreadContext c{};
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(Sys(m, 130, {q, 0, 2'000'000}, &c), CELL_OK);
        CHECK_EQ(c.gpr[4], 0x77);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(ms > 10.0 && ms < 2000.0);  // ~20 ms nominal
    // Process end with the timer still running: Shutdown joins the timer thread.
    m.kernel->Shutdown();
}

void TestPadNoController() {
    std::printf("-- cellPad (sys_io): init/end, info structures, no device on any port\n");
    Machine m;
    hle::CellPad pad;
    pad.Register(*m.hle);
    CHECK_EQ(Call(m, "sys_io", "cellPadGetInfo2", {kOut}), hle::CELL_PAD_ERROR_UNINITIALIZED);
    CHECK_EQ(Call(m, "sys_io", "cellPadEnd", {}), hle::CELL_PAD_ERROR_UNINITIALIZED);
    CHECK_EQ(Call(m, "sys_io", "cellPadInit", {0}), hle::CELL_PAD_ERROR_INVALID_PARAMETER);
    CHECK_EQ(Call(m, "sys_io", "cellPadInit", {128}), hle::CELL_PAD_ERROR_INVALID_PARAMETER);
    CHECK_EQ(Call(m, "sys_io", "cellPadInit", {7}), CELL_OK);  // what GT5 passes
    CHECK_EQ(Call(m, "sys_io", "cellPadInit", {7}), hle::CELL_PAD_ERROR_ALREADY_INITIALIZED);

    CHECK_EQ(Call(m, "sys_io", "cellPadSetPortSetting", {0, 0x6}), CELL_OK);
    CHECK_EQ(Call(m, "sys_io", "cellPadSetPortSetting", {200, 0x6}), hle::CELL_PAD_ERROR_INVALID_PARAMETER);
    CHECK_EQ(Call(m, "sys_io", "cellPadSetPortSetting", {50, 0x6}), CELL_OK);  // beyond 7 ports: accepted, ignored
    CHECK_EQ(Call(m, "sys_io", "cellPadSetPressMode", {1, 2}), hle::CELL_PAD_ERROR_INVALID_PARAMETER);
    CHECK_EQ(Call(m, "sys_io", "cellPadSetPressMode", {1, 1}), CELL_OK);
    CHECK_EQ(Call(m, "sys_io", "cellPadSetSensorMode", {2, 1}), CELL_OK);
    CHECK_EQ(Call(m, "sys_io", "cellPadSetSensorMode", {2, 0}), CELL_OK);

    m.memory.Fill(kOut, 0xEE, 130);
    CHECK_EQ(Call(m, "sys_io", "cellPadGetInfo2", {kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut + 0), 7);  // max_connect
    CHECK_EQ(m.memory.Read<u32>(kOut + 4), 0);  // now_connect
    CHECK_EQ(m.memory.Read<u32>(kOut + 8), 0);  // system_info
    bool none = true;
    for (u32 i = 0; i < 7; ++i) none &= m.memory.Read<u32>(kOut + 12 + 4 * i) == 0;  // port_status
    CHECK(none);
    CHECK_EQ(m.memory.Read<u32>(kOut + 40), 0x6);  // port_setting[0]
    CHECK_EQ(m.memory.Read<u32>(kOut + 44), 0x2);  // port_setting[1]: press mode
    CHECK_EQ(m.memory.Read<u32>(kOut + 48), 0x0);  // port_setting[2]: sensor on then off
    CHECK_EQ(m.memory.Read<u32>(kOut + 68), 0);    // device_capability[0]
    CHECK_EQ(m.memory.Read<u8>(kOut + 124), 0xEE); // 124 bytes only
    CHECK_EQ(Call(m, "sys_io", "cellPadGetInfo2", {0}), hle::CELL_PAD_ERROR_INVALID_PARAMETER);

    const u32 info = kData + 0x1000;
    m.memory.Fill(info, 0xEE, 660);
    CHECK_EQ(Call(m, "sys_io", "cellPadGetInfo", {info}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(info), 7);
    CHECK_EQ(m.memory.Read<u32>(info + 4), 0);
    CHECK_EQ(m.memory.Read<u8>(info + 12 + 254 + 254), 0);  // status[0]
    CHECK_EQ(m.memory.Read<u8>(info + 646), 0);
    CHECK_EQ(m.memory.Read<u8>(info + 647), 0xEE);

    const u32 data = kData + 0x2000;
    m.memory.Fill(data, 0xEE, 132);
    CHECK_EQ(Call(m, "sys_io", "cellPadGetData", {0, data}), hle::CELL_PAD_ERROR_NO_DEVICE);
    CHECK_EQ(m.memory.Read<u32>(data), 0xEEEEEEEEu);  // nothing fabricated
    CHECK_EQ(Call(m, "sys_io", "cellPadGetData", {6, data}), hle::CELL_PAD_ERROR_NO_DEVICE);
    CHECK_EQ(Call(m, "sys_io", "cellPadGetData", {127, data}), hle::CELL_PAD_ERROR_INVALID_PARAMETER);
    CHECK_EQ(Call(m, "sys_io", "cellPadGetData", {0, 0}), hle::CELL_PAD_ERROR_INVALID_PARAMETER);
    CHECK_EQ(Call(m, "sys_io", "cellPadGetCapabilityInfo", {0, data}), hle::CELL_PAD_ERROR_NO_DEVICE);
    CHECK_EQ(Call(m, "sys_io", "cellPadClearBuf", {0}), hle::CELL_PAD_ERROR_NO_DEVICE);
    CHECK_EQ(Call(m, "sys_io", "cellPadEnd", {}), CELL_OK);
    CHECK_EQ(Call(m, "sys_io", "cellPadGetData", {0, data}), hle::CELL_PAD_ERROR_UNINITIALIZED);
    CHECK_EQ(Call(m, "sys_io", "cellPadInit", {7}), CELL_OK);  // re-init after end
}

void TestCondSemantics() {
    std::printf("-- lv2 cond: one wake per signal, signal_all, unmatched signals counted, timeout, EPERM\n");
    Machine m;
    auto mx = std::make_shared<lv2::Lv2Mutex>();
    m.kernel->objects().Add(lv2::kIdMutex, mx);
    auto cond = std::make_shared<lv2::Lv2Cond>();
    cond->mutex = mx;
    m.kernel->objects().Add(lv2::kIdCond, cond);
    lv2::Kernel& k = *m.kernel;

    CHECK_EQ(cond->Wait(k, 5, 0), CELL_EPERM);  // not the owner
    CHECK_EQ(cond->Signal(9), CELL_OK);          // nobody waits: counted, not remembered
    std::atomic<int> woken{0};
    std::vector<std::thread> waiters;
    for (u32 tid : {1u, 2u, 3u})
        waiters.emplace_back([&, tid] {
            CHECK_EQ(mx->Lock(k, tid, 0), CELL_OK);
            CHECK_EQ(cond->Wait(k, tid, 0), CELL_OK);
            CHECK_EQ(mx->owner, tid);  // returns with the mutex held
            ++woken;
            CHECK_EQ(mx->Unlock(tid), CELL_OK);
        });
    auto waiting = [&] {
        std::lock_guard lock(mx->m);
        return cond->waiters.size();
    };
    for (int i = 0; i < 2000 && waiting() < 3; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK_EQ(waiting(), 3);
    CHECK_EQ(woken.load(), 0);  // the earlier signal did not leak into a later wait
    CHECK_EQ(cond->Signal(9), CELL_OK);
    for (int i = 0; i < 2000 && woken.load() < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(woken.load(), 1);  // exactly one
    CHECK_EQ(cond->SignalAll(9), CELL_OK);
    for (auto& t : waiters) t.join();
    CHECK_EQ(woken.load(), 3);
    {
        std::lock_guard lock(mx->m);
        CHECK_EQ(cond->waits, 3);
        CHECK_EQ(cond->signals, 3);
        CHECK_EQ(cond->unmatched_signals, 1);
        CHECK_EQ(cond->last_signaler, 9);
    }
    CHECK(cond->Stats().find("3 wait(s), 3 signal(s) (1 found no waiter)") != std::string::npos);
    CHECK(cond->Stats().find("by 0x00000009") != std::string::npos);

    CHECK_EQ(mx->Lock(k, 4, 0), CELL_OK);
    CHECK_EQ(cond->Wait(k, 4, 2000), CELL_ETIMEDOUT);
    CHECK_EQ(mx->owner, 4);  // re-held after the timeout
    CHECK_EQ(cond->SignalTo(4, 9), CELL_EPERM);  // no longer waiting
    CHECK_EQ(mx->Unlock(4), CELL_OK);
}

bool Has(const std::string& text, const char* what) {
    if (text.find(what) != std::string::npos) return true;
    std::fprintf(stderr, "report lacks \"%s\":\n%s\n", what, text.c_str());
    return false;
}

// Guest thread: sys_lwmutex_lock(lw, 0); sys_lwcond_wait(lc, 0); sys_lwmutex_unlock(lw); return.
void TestStallDecoding() {
    std::printf("-- stall report: sys_lwcond_wait decoded; queue producers listed\n");
    Machine m;
    using namespace as;
    const u32 lw = kData + 0x400, lc = kData + 0x420, attr = kData + 0x440;
    m.memory.Fill(attr, 0, 16);
    m.memory.Write<u32>(attr, lv2::SYS_SYNC_PRIORITY);
    m.memory.Write<u32>(attr + 4, lv2::SYS_SYNC_NOT_RECURSIVE);
    CHECK_EQ(Call(m, "sysPrxForUser", "sys_lwmutex_create", {lw, attr}), CELL_OK);
    CHECK_EQ(Call(m, "sysPrxForUser", "sys_lwcond_create", {lc, lw, attr + 8}), CELL_OK);
    const u32 lock_opd = m.ImportOpd("sysPrxForUser", "sys_lwmutex_lock");
    const u32 wait_opd = m.ImportOpd("sysPrxForUser", "sys_lwcond_wait");
    const u32 unlock_opd = m.ImportOpd("sysPrxForUser", "sys_lwmutex_unlock");
    auto call = [](u32 opd, u32 arg) {
        return std::vector<u32>{lis(3, static_cast<s16>(arg >> 16)), ori(3, 3, arg & 0xFFFF), li(4, 0),
                                lis(12, static_cast<s16>(opd >> 16)), ori(12, 12, opd & 0xFFFF), lwz(0, 0, 12), mtctr(0), bctrl()};
    };
    std::vector<u32> code{mflr(31)};
    for (u32 w : call(lock_opd, lw)) code.push_back(w);
    for (u32 w : call(wait_opd, lc)) code.push_back(w);
    const u32 after_wait = kCode + 0x800 + static_cast<u32>(code.size()) * 4;
    for (u32 w : call(unlock_opd, lw)) code.push_back(w);
    code.insert(code.end(), {mtlr(31), blr()});
    m.Load(code, kCode + 0x800);
    m.memory.Write<u32>(kData + 0x500, kCode + 0x800);
    m.memory.Write<u32>(kData + 0x504, 0);
    lv2::ThreadManager::CreateParams p;
    p.entry_opd = kData + 0x500;
    p.flags = lv2::SYS_PPU_THREAD_CREATE_JOINABLE;
    p.name = "waiter";
    u32 id = 0;
    CHECK_EQ(m.kernel->threads().Create(p, &id), CELL_OK);
    CHECK_EQ(m.kernel->threads().Start(id), CELL_OK);
    bool blocked = false;
    for (int i = 0; i < 2000 && !blocked; ++i) {
        for (const auto& t : m.kernel->threads().Snapshot())
            if (t.id == id && t.live_kind == ppu::PpuLiveStatus::kInHle && m.hle->At(t.live_call).name == "sys_lwcond_wait") blocked = true;
        if (!blocked) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(blocked);

    lv2::StallMonitor monitor(*m.kernel, 0);
    const std::string r = monitor.BuildReport("test");
    char text[160];
    std::snprintf(text, sizeof(text), "BLOCKED in HLE sysPrxForUser::sys_lwcond_wait(0x%x, 0x0)\n", lc);  // two arguments only
    CHECK(Has(r, text));
    std::snprintf(text, sizeof(text), "lwcond 0x%08x (guest struct): lwmutex 0x%08x, cond id 0x97", lc, lw);
    CHECK(Has(r, text));
    CHECK(Has(r, "no timeout"));
    CHECK(Has(r, "1 waiter(s), 1 wait(s), 0 signal(s) (0 found no waiter), never signaled"));
    std::snprintf(text, sizeof(text), "lwmutex 0x%08x (guest struct): owner 0xffffffff", lw);  // released while waiting
    CHECK(Has(r, text));
    std::snprintf(text, sizeof(text), "caller: 0x%08x: bctrl", after_wait - 4);
    CHECK(Has(r, text));
    // Phase 7.7: nobody ever signaled it -> no known producer; the calls before the wait.
    CHECK(Has(r, "expected producer: unknown — no thread has ever signaled this condition"));
    std::snprintf(text, sizeof(text), "before: HLE sysPrxForUser::sys_lwmutex_lock(0x%x, 0x0) -> 0x0", lw);
    CHECK(Has(r, text));

    CHECK_EQ(Call(m, "sysPrxForUser", "sys_lwcond_signal", {lc}, 0x01000099), CELL_OK);
    ppu::PpuThreadContext host{};
    u64 exit_value = 0;
    CHECK_EQ(m.kernel->threads().Join(host, id, &exit_value), CELL_OK);
    CHECK_EQ(static_cast<s32>(exit_value), CELL_OK);  // r3 of the unlock
    const auto cond = m.kernel->objects().Get<lv2::Lv2Cond>(m.memory.Read<u32>(lc + 4));
    CHECK(cond && cond->Stats().find("by 0x01000099") != std::string::npos);
    // Same wait again: now the report names the thread that signaled it last time.
    p.name = "waiter2";
    CHECK_EQ(m.kernel->threads().Create(p, &id), CELL_OK);
    CHECK_EQ(m.kernel->threads().Start(id), CELL_OK);
    blocked = false;
    for (int i = 0; i < 2000 && !blocked; ++i) {
        for (const auto& t : m.kernel->threads().Snapshot())
            if (t.id == id && t.live_kind == ppu::PpuLiveStatus::kInHle && m.hle->At(t.live_call).name == "sys_lwcond_wait") blocked = true;
        if (!blocked) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(blocked);
    const std::string r1 = monitor.BuildReport("test");
    CHECK(Has(r1, "past signaler 0x01000099: no longer registered"));
    CHECK_EQ(Call(m, "sysPrxForUser", "sys_lwcond_signal", {lc}, 0x01000099), CELL_OK);
    CHECK_EQ(m.kernel->threads().Join(host, id, &exit_value), CELL_OK);
    p.name = "waiter";

    // A thread in sys_event_queue_receive: the queue's producers are listed.
    m.kernel->timers().set_realtime(false);
    const u32 q = MakeQueue(m);
    CHECK_EQ(Sys(m, 70, {kOut}), CELL_OK);
    const u32 timer = m.memory.Read<u32>(kOut);
    CHECK_EQ(Sys(m, 75, {timer, q, 0, 0, 0}), CELL_OK);
    const u32 code2 = kCode + 0xA00;
    m.Load({lis(3, static_cast<s16>(q >> 16)), ori(3, 3, q & 0xFFFF), li(4, 0), li(5, 0), li(11, 130), sc(), blr()}, code2);
    m.code->Invalidate(code2, 0x100);  // same page as the first thread's code: drop its pre-decoded copy
    m.memory.Write<u32>(kData + 0x510, code2);
    m.memory.Write<u32>(kData + 0x514, 0);
    p.entry_opd = kData + 0x510;
    p.name = "receiver";
    CHECK_EQ(m.kernel->threads().Create(p, &id), CELL_OK);
    CHECK_EQ(m.kernel->threads().Start(id), CELL_OK);
    blocked = false;
    for (int i = 0; i < 2000 && !blocked; ++i) {
        for (const auto& t : m.kernel->threads().Snapshot())
            if (t.id == id && t.live_kind == ppu::PpuLiveStatus::kInSyscall && t.live_call == 130) blocked = true;
        if (!blocked) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(blocked);
    const std::string r2 = monitor.BuildReport("test");
    std::snprintf(text, sizeof(text), "producer: timer 0x%08x: stopped, period 0 us, connected to queue 0x%08x", timer, q);
    CHECK(Has(r2, text));
    CHECK(Has(r2, "0 received in total (last never); no timeout"));
    CHECK_EQ(Sys(m, 129, {q, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE}), CELL_OK);  // wake it
    CHECK_EQ(m.kernel->threads().Join(host, id, &exit_value), CELL_OK);
}

}  // namespace

int main() {
    log::Threshold() = log::Level::Error;
    TestTimerSyscalls();
    TestTimerRealtime();
    TestPadNoController();
    TestCondSemantics();
    TestStallDecoding();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all timer / pad / sync tests passed\n");
    return 0;
}
