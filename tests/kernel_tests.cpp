// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/kernel_tests.cpp
//
// Phase 3: lv2 kernel concurrency. Guest programs create real PPU threads (host threads),
// contend on lv2 mutexes and lwmutexes, pass events through queues/ports, and end the process
// while other threads are blocked. Host-level tests cover the sync objects' edge cases.
// Build with -fsanitize=thread to check the host side for races.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "hle/fnid.h"
#include "lv2/sys_event.h"
#include "lv2/sys_spu.h"
#include "lv2/sys_sync.h"
#include "ppu_asm.h"
#include "test_machine.h"

using namespace cell;

namespace {

constexpr u32 add(u32 rt, u32 ra, u32 rb) { return as::XO(rt, ra, rb, 266); }
constexpr u32 mulli(u32 rt, u32 ra, s32 v) { return as::D(7, rt, ra, v); }

// Data layout (offsets from kData)
constexpr s32 kSlotThreadCreate = 0x000, kSlotLwCreate = 0x004, kSlotLwLock = 0x008, kSlotLwUnlock = 0x00C;
constexpr s32 kChildOpd = 0x100, kName = 0x200, kTids = 0x300, kStatus = 0x340, kCounter = 0x400;
constexpr s32 kMutexAttr = 0x500, kMutexId = 0x540, kQueueAttr = 0x600, kQueueId = 0x620, kPortId = 0x624;
constexpr s32 kLwMutex = 0x700, kLwAttr = 0x720;
constexpr u32 kChildCode = kCode + 0x800;

// Fails the run instead of hanging forever if a wake-up is lost.
class Watchdog {
public:
    explicit Watchdog(const char* what, int seconds = 20) : what_(what) {
        thread_ = std::thread([this, seconds] {
            for (int i = 0; i < seconds * 10 && !done_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!done_.load()) {
                std::fprintf(stderr, "FAIL watchdog: \"%s\" hung (lost wake-up / deadlock)\n", what_);
                std::_Exit(2);
            }
        });
    }
    ~Watchdog() {
        done_ = true;
        thread_.join();
    }

private:
    const char* what_;
    std::atomic<bool> done_{false};
    std::thread thread_;
};

void SetupCommon(Machine& m) {
    m.memory.Write<u32>(kData + kSlotThreadCreate, m.ImportOpd("sysPrxForUser", "sys_ppu_thread_create"));
    m.memory.Write<u32>(kData + kSlotLwCreate, m.ImportOpd("sysPrxForUser", "sys_lwmutex_create"));
    m.memory.Write<u32>(kData + kSlotLwLock, m.ImportOpd("sysPrxForUser", "sys_lwmutex_lock"));
    m.memory.Write<u32>(kData + kSlotLwUnlock, m.ImportOpd("sysPrxForUser", "sys_lwmutex_unlock"));
    m.memory.Write<u32>(kData + kChildOpd, kChildCode);  // OPD {code, toc}
    m.memory.Write<u32>(kData + kChildOpd + 4, 0);
    m.memory.CopyIn(kData + kName, "worker", 7);
}

// Main-thread fragment: sys_ppu_thread_create(&tids[r25], child, arg = r26, 1001, 64K, joinable, "worker")
void EmitCreate(Program& p) {
    using namespace as;
    p << mulli(3, 25, 8) << add(3, 3, 30) << addi(3, 3, kTids) << addi(4, 30, kChildOpd) << mr(5, 26) << li(6, 1001)
      << lis(7, 1) << li(8, 1) << addi(9, 30, kName);
    p.call_import(30, kSlotThreadCreate);
}
// Main-thread fragment: sys_ppu_thread_join(tids[r25], &status) -> r3, status in [kStatus]
void EmitJoin(Program& p) {
    using namespace as;
    p << mulli(3, 25, 8) << add(3, 3, 30) << ld(3, kTids, 3) << addi(4, 30, kStatus);
    p.syscall(44);
}

// ---------------------------------------------------------------------------------------
void TestThreadCreateJoin() {
    std::printf("-- sys_ppu_thread_create + join: child returns arg*2+1\n");
    Watchdog dog("create/join");
    using namespace as;
    Machine m;
    SetupCommon(m);
    m.Load({mulli(3, 3, 2), addi(3, 3, 1), blr()}, kChildCode);
    Program p;
    p << mflr(31) << lis(30, 2) << li(25, 0) << li(26, 5);
    EmitCreate(p);
    p << mr(20, 3);
    EmitJoin(p);
    p << mr(21, 3) << ld(22, kStatus, 30) << mtlr(31) << blr();
    const auto stop = m.RunProcess(p.words());
    CHECK(stop.reason == ppu::StopReason::Exited);
    CHECK_EQ(m.ctx.gpr[20], 0);
    CHECK_EQ(m.ctx.gpr[21], 0);
    CHECK_EQ(m.ctx.gpr[22], 11);
    CHECK(!m.kernel->process_end().fatal);
    // Joined thread reaped: only the primary remains.
    CHECK_EQ(m.kernel->threads().Snapshot().size(), 1);
}

// ---------------------------------------------------------------------------------------
// N threads x K increments of a shared counter, each under sys_mutex_lock (syscall 102/104)
// or sys_lwmutex_lock (HLE). Lost updates would show up as a smaller total.
void RunCounterContention(bool lw) {
    std::printf("-- %d threads contend on %s: counter must be exact\n", 4, lw ? "an lwmutex (HLE)" : "an lv2 mutex (syscalls)");
    Watchdog dog(lw ? "lwmutex contention" : "mutex contention");
    using namespace as;
    // Sanitizer builds are 10-50x slower: CELLOS_TEST_ITERATIONS shrinks the loop for them.
    constexpr s32 kThreads = 4;
    const char* env = std::getenv("CELLOS_TEST_ITERATIONS");
    const s32 kIterations = env ? std::max(1, std::atoi(env)) : 200000;
    Machine m;
    SetupCommon(m);
    m.memory.Write<u32>(kData + kMutexAttr, lv2::SYS_SYNC_PRIORITY);
    m.memory.Write<u32>(kData + kMutexAttr + 4, lv2::SYS_SYNC_NOT_RECURSIVE);
    m.memory.Write<u32>(kData + kLwAttr, lv2::SYS_SYNC_PRIORITY);
    m.memory.Write<u32>(kData + kLwAttr + 4, lv2::SYS_SYNC_NOT_RECURSIVE);

    Program child;
    // The HLE calls (bctrl) clobber LR: keep the return address like compiled code does.
    child << mflr(31) << lis(30, 2) << lwz(29, kMutexId, 30) << lis(28, kIterations >> 16) << ori(28, 28, kIterations & 0xFFFF);
    child.label("loop");
    if (lw) {
        child << addi(3, 30, kLwMutex) << li(4, 0);
        child.call_import(30, kSlotLwLock);
    } else {
        child << mr(3, 29) << li(4, 0);
        child.syscall(102);
    }
    child << lwz(5, kCounter, 30) << addi(5, 5, 1) << stw(5, kCounter, 30);
    if (lw) {
        child << addi(3, 30, kLwMutex);
        child.call_import(30, kSlotLwUnlock);
    } else {
        child << mr(3, 29);
        child.syscall(104);
    }
    child << addi(28, 28, -1) << cmpwi(0, 28, 0);
    child.bne(0, "loop");
    child << li(3, 0) << mtlr(31) << blr();
    m.Load(child.words(), kChildCode);

    Program p;
    p << mflr(31) << lis(30, 2);
    if (lw) {
        p << addi(3, 30, kLwMutex) << addi(4, 30, kLwAttr);
        p.call_import(30, kSlotLwCreate);
    } else {
        p << addi(3, 30, kMutexId) << addi(4, 30, kMutexAttr);
        p.syscall(100);
    }
    p << mr(19, 3) << li(25, 0);
    p.label("create");
    p << mr(26, 25);
    EmitCreate(p);
    p << addi(25, 25, 1) << cmpwi(0, 25, kThreads);
    p.bne(0, "create");
    p << li(25, 0);
    p.label("join");
    EmitJoin(p);
    p << addi(25, 25, 1) << cmpwi(0, 25, kThreads);
    p.bne(0, "join");
    p << lwz(22, kCounter, 30) << mtlr(31) << blr();

    const auto stop = m.RunProcess(p.words());
    CHECK(stop.reason == ppu::StopReason::Exited);
    CHECK_EQ(m.ctx.gpr[19], 0);
    CHECK_EQ(m.ctx.gpr[22], kThreads * kIterations);
    CHECK(!m.kernel->process_end().fatal);
    if (lw) CHECK_EQ(m.memory.Read<u32>(kData + kLwMutex), lv2::kLwmutexFree);  // guest mirror: unlocked
}

// ---------------------------------------------------------------------------------------
void TestEventQueue() {
    std::printf("-- event queue + port: 1000 events PPU->PPU, sum checked, backpressure on a full queue\n");
    Watchdog dog("event queue");
    using namespace as;
    constexpr s32 kEvents = 1000;
    Machine m;
    SetupCommon(m);
    m.memory.Write<u32>(kData + kQueueAttr, lv2::SYS_SYNC_FIFO);
    m.memory.Write<u32>(kData + kQueueAttr + 4, lv2::SYS_PPU_QUEUE);
    m.memory.CopyIn(kData + kQueueAttr + 8, "evq", 4);

    // Consumer: receive arg times (event comes back in r4..r7), return the sum of data1.
    Program child;
    child << lis(30, 2) << lwz(29, kQueueId, 30) << mr(28, 3) << li(27, 0);
    child.label("recv");
    child << mr(3, 29) << li(4, 0) << li(5, 0);
    child.syscall(130);
    child << add(27, 27, 5) << addi(28, 28, -1) << cmpwi(0, 28, 0);
    child.bne(0, "recv");
    child << mr(3, 27) << blr();
    m.Load(child.words(), kChildCode);

    Program p;
    p << mflr(31) << lis(30, 2);
    p << addi(3, 30, kQueueId) << addi(4, 30, kQueueAttr) << li(5, 0) << li(6, 16);  // small queue: forces EBUSY
    p.syscall(128);
    p << mr(18, 3) << addi(3, 30, kPortId) << li(4, 1) << li(5, 0);
    p.syscall(134);
    p << lwz(3, kPortId, 30) << lwz(4, kQueueId, 30);
    p.syscall(136);
    p << mr(19, 3) << li(25, 0) << li(26, kEvents);
    EmitCreate(p);
    p << li(24, 1);
    p.label("send");
    p << lwz(3, kPortId, 30) << mr(4, 24) << li(5, 0) << li(6, 0);
    p.syscall(138);
    p << cmpwi(0, 3, 0);
    p.beq(0, "sent");
    p.syscall(43);  // queue full: yield and retry
    p.b("send");
    p.label("sent");
    p << addi(24, 24, 1) << cmpwi(0, 24, kEvents + 1);
    p.bne(0, "send");
    p << li(25, 0);
    EmitJoin(p);
    p << ld(22, kStatus, 30) << mtlr(31) << blr();

    const auto stop = m.RunProcess(p.words());
    CHECK(stop.reason == ppu::StopReason::Exited);
    CHECK_EQ(m.ctx.gpr[18], 0);
    CHECK_EQ(m.ctx.gpr[19], 0);
    CHECK_EQ(m.ctx.gpr[22], kEvents * (kEvents + 1) / 2);
}

// ---------------------------------------------------------------------------------------
void TestTerminationWakesBlockedThreads() {
    std::printf("-- sys_process_exit while workers block in receive/lock/sleep: no hang\n");
    Watchdog dog("termination");
    using namespace as;
    Machine m;
    SetupCommon(m);
    m.memory.Write<u32>(kData + kQueueAttr, lv2::SYS_SYNC_FIFO);
    m.memory.Write<u32>(kData + kQueueAttr + 4, lv2::SYS_PPU_QUEUE);
    // Worker: block forever on an empty queue.
    Program child;
    child << lis(30, 2) << lwz(3, kQueueId, 30) << li(4, 0) << li(5, 0);
    child.syscall(130);
    child << li(3, 99) << blr();
    m.Load(child.words(), kChildCode);

    Program p;
    p << mflr(31) << lis(30, 2) << addi(3, 30, kQueueId) << addi(4, 30, kQueueAttr) << li(5, 0) << li(6, 8);
    p.syscall(128);
    p << li(25, 0) << li(26, 0);
    EmitCreate(p);
    p << li(25, 1);
    EmitCreate(p);
    p << li(3, 2000);
    p.syscall(141);           // usleep: let the workers block
    p << li(3, 7);
    p.syscall(22);            // _sys_process_exit(7)
    p << li(20, 1) << blr();  // never reached
    const auto stop = m.RunProcess(p.words());
    CHECK(stop.reason == ppu::StopReason::Exited);
    const auto end = m.kernel->process_end();
    CHECK(end.ended && !end.fatal);
    CHECK_EQ(end.kind, ppu::kExitProcess);
    CHECK_EQ(end.status, 7);
    CHECK(m.ctx.gpr[20] != 1);
}

// ---------------------------------------------------------------------------------------
void TestFatalInWorkerEndsProcess() {
    std::printf("-- a fault in a worker thread ends the process with a report\n");
    Watchdog dog("fatal worker");
    using namespace as;
    Machine m;
    SetupCommon(m);
    m.Load({li(3, 0), lwz(4, 0x20, 3), blr()}, kChildCode);  // null-page read
    Program p;
    p << mflr(31) << lis(30, 2) << li(25, 0) << li(26, 0);
    EmitCreate(p);
    EmitJoin(p);  // never returns normally: the process ends under it
    p << mtlr(31) << blr();
    m.RunProcess(p.words());
    const auto end = m.kernel->process_end();
    CHECK(end.fatal);
    CHECK(end.stop.reason == ppu::StopReason::DataFault);
    CHECK_EQ(end.stop.address, 0x20);
    CHECK_EQ(end.stop.pc, kChildCode + 4);
    CHECK(end.thread_id != lv2::kIdPpuThread);
}

// ---------------------------------------------------------------------------------------
// Host-level tests of the sync objects (edge cases that are awkward to reach from guest code).
void TestSyncObjectsHostLevel() {
    std::printf("-- sync object semantics (timeouts, recursion, cond, semaphore, event flag)\n");
    Watchdog dog("host-level sync");
    Machine m;
    lv2::Kernel& k = *m.kernel;

    lv2::Lv2Mutex mx;
    CHECK_EQ(mx.Lock(k, 1, 0), CELL_OK);
    CHECK_EQ(mx.Lock(k, 1, 0), CELL_EDEADLK);  // not recursive
    CHECK_EQ(mx.TryLock(2), CELL_EBUSY);
    CHECK_EQ(mx.Lock(k, 2, 2000), CELL_ETIMEDOUT);
    CHECK_EQ(mx.Unlock(2), CELL_EPERM);
    CHECK_EQ(mx.Unlock(1), CELL_OK);
    mx.recursive = true;
    CHECK_EQ(mx.Lock(k, 3, 0), CELL_OK);
    CHECK_EQ(mx.Lock(k, 3, 0), CELL_OK);
    CHECK_EQ(mx.Unlock(3), CELL_OK);
    CHECK_EQ(mx.owner, 3);
    CHECK_EQ(mx.Unlock(3), CELL_OK);
    CHECK_EQ(mx.owner, 0);

    // Cond: waiter releases the mutex, signal hands it back; timeout keeps the mutex owned.
    auto shared = std::make_shared<lv2::Lv2Mutex>();
    lv2::Lv2Cond cond;
    cond.mutex = shared;
    CHECK_EQ(cond.Wait(k, 5, 0), CELL_EPERM);  // not the owner
    CHECK_EQ(shared->Lock(k, 5, 0), CELL_OK);
    CHECK_EQ(cond.Wait(k, 5, 1000), CELL_ETIMEDOUT);
    CHECK_EQ(shared->owner, 5);
    std::atomic<bool> waiting{false};
    s32 waiter_result = -1;
    std::thread waiter([&] {
        waiting = true;
        waiter_result = cond.Wait(k, 5, 0);  // releases thread 5's lock while waiting
    });
    while (!waiting) std::this_thread::yield();
    // Thread 6 can take the mutex only once the waiter released it.
    CHECK_EQ(shared->Lock(k, 6, 2'000'000), CELL_OK);
    CHECK_EQ(cond.SignalTo(77), CELL_EPERM);
    CHECK_EQ(cond.Signal(), CELL_OK);
    CHECK_EQ(shared->Unlock(6), CELL_OK);
    waiter.join();
    CHECK_EQ(waiter_result, CELL_OK);
    CHECK_EQ(shared->owner, 5);

    lv2::Lv2Semaphore sem;
    sem.count = 0;
    sem.max = 2;
    CHECK_EQ(sem.TryWait(), CELL_EBUSY);
    CHECK_EQ(sem.Wait(k, 1000), CELL_ETIMEDOUT);
    CHECK_EQ(sem.Post(3), CELL_EBUSY);
    CHECK_EQ(sem.Post(2), CELL_OK);
    CHECK_EQ(sem.Wait(k, 0), CELL_OK);
    CHECK_EQ(sem.count, 1);

    lv2::Lv2EventFlag ef;
    ef.type = lv2::SYS_SYNC_WAITER_MULTIPLE;
    u64 result = 0;
    CHECK_EQ(ef.TryWait(0x3, lv2::SYS_EVENT_FLAG_WAIT_AND, &result), CELL_EBUSY);
    ef.Set(0x1);
    CHECK_EQ(ef.TryWait(0x3, lv2::SYS_EVENT_FLAG_WAIT_OR | lv2::SYS_EVENT_FLAG_WAIT_CLEAR, &result), CELL_OK);
    CHECK_EQ(result, 0x1);
    CHECK_EQ(ef.pattern, 0);
    CHECK_EQ(ef.TryWait(0, lv2::SYS_EVENT_FLAG_WAIT_AND, &result), CELL_EINVAL);
    s32 flag_result = -1;
    std::thread flag_waiter([&] {
        u64 r = 0;
        flag_result = ef.Wait(k, 0x6, lv2::SYS_EVENT_FLAG_WAIT_AND | lv2::SYS_EVENT_FLAG_WAIT_CLEAR_ALL, &r, 0);
    });
    ef.Set(0x2);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ef.Set(0x4);  // now AND is satisfied
    flag_waiter.join();
    CHECK_EQ(flag_result, CELL_OK);
    CHECK_EQ(ef.pattern, 0);  // CLEAR_ALL
    std::thread canceled([&] {
        u64 r = 0;
        flag_result = ef.Wait(k, 0x8, lv2::SYS_EVENT_FLAG_WAIT_AND, &r, 0);
    });
    while (ef.Cancel() == 0) std::this_thread::yield();  // retry until the waiter is queued
    canceled.join();
    CHECK_EQ(flag_result, CELL_ECANCELED);

    // Forced queue destroy releases a blocked receiver with ECANCELED.
    auto q = std::make_shared<lv2::Lv2EventQueue>();
    q->size = 2;
    CHECK_EQ(q->Push({1, 2, 3, 4}), CELL_OK);
    CHECK_EQ(q->Push({1, 2, 3, 4}), CELL_OK);
    CHECK_EQ(q->Push({1, 2, 3, 4}), CELL_EBUSY);
    lv2::Lv2Event e{};
    CHECK_EQ(q->Receive(k, 0, &e), CELL_OK);
    CHECK_EQ(e.data3, 4);
    CHECK_EQ(q->Receive(k, 0, &e), CELL_OK);
    CHECK_EQ(q->Receive(k, 1000, &e), CELL_ETIMEDOUT);
    s32 receive_result = -1;
    std::thread receiver([&] {
        lv2::Lv2Event ev{};
        receive_result = q->Receive(k, 0, &ev);
    });
    for (;;) {
        std::lock_guard lock(q->m);
        if (q->waiters) {
            q->destroyed = true;
            q->cv.notify_all();
            break;
        }
    }
    receiver.join();
    CHECK_EQ(receive_result, CELL_ECANCELED);
}

void TestSysmodule() {
    std::printf("-- cellSysmodule bookkeeping\n");
    Machine m;
    CHECK_EQ(m.sysmodule->IsLoaded(0x2E), false);
    CHECK_EQ(m.sysmodule->Load(0x2E), CELL_OK);
    CHECK(m.sysmodule->IsLoaded(0x2E));
    CHECK_EQ(m.sysmodule->Unload(0x2E), CELL_OK);
    CHECK_EQ(m.sysmodule->Unload(0x2E), hle::CELL_SYSMODULE_ERROR_UNLOADED);
    CHECK(std::string(hle::CellSysmodule::ModuleName(0x0E)) == "FS");
}


void TestSpuInitialize() {
    std::printf("-- syscall 169 sys_spu_initialize\n");
    using namespace as;
    Machine m;
    Program p;
    p << li(3, 6) << li(4, 1);
    p.syscall(169);
    p << mr(20, 3) << li(3, 6) << li(4, 6);  // 6 raw SPUs: impossible
    p.syscall(169);
    p << mr(21, 3) << blr();
    const auto stop = m.Run(p.words());
    CHECK(stop.reason == ppu::StopReason::Exited);
    CHECK_EQ(m.ctx.gpr[20], 0);
    CHECK_EQ(static_cast<s32>(m.ctx.gpr[21]), CELL_EINVAL);
    const auto limits = m.kernel->spu().limits();
    CHECK(limits.initialized && limits.max_usable_spu == 6 && limits.max_raw_spu == 1);
    CHECK(lv2::KnownSyscallName(170) && std::string(lv2::KnownSyscallName(170)) == "sys_spu_thread_group_create");
}

}  // namespace

int main() {
    log::Threshold() = log::Level::Warn;
    TestThreadCreateJoin();
    RunCounterContention(false);
    RunCounterContention(true);
    TestEventQueue();
    TestTerminationWakesBlockedThreads();
    TestFatalInWorkerEndsProcess();
    TestSyncObjectsHostLevel();
    TestSysmodule();
    TestSpuInitialize();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all kernel tests passed\n");
    return 0;
}
