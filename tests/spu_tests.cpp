// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/spu_tests.cpp
//
// Phase 4: SPU runtime foundation. There is no SPU instruction decoder yet, so SPU programs
// are C++ scripts run by a test ISpuExecutor: they use exactly the interface the future
// interpreter/JIT will use (ReadChannel / WriteChannel / ChannelCount / StopAndSignal on real
// SPU threads running on host threads), against the real lv2 syscalls driven from the PPU side.
// Build with -fsanitize=thread to check SPU<->PPU synchronization for races.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include "hle/cell_spurs.h"
#include "hle/fnid.h"
#include "hle/sys_spu_hle.h"
#include "lv2/sys_event.h"
#include "lv2/sys_spu.h"
#include "lv2/sys_sync.h"
#include "spu/spu_mfc.h"
#include "test_machine.h"

using namespace cell;
using spu::SpuThread;

namespace {

// Checks made on SPU host threads (g_failures is not atomic).
std::atomic<int> g_spu_failures{0};
#define SCHECK(cond)                                                                   \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL (SPU) %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_spu_failures.fetch_add(1);                                               \
        }                                                                              \
    } while (0)

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

// ---------------------------------------------------------------------------------------
// Scripted SPU executor
// ---------------------------------------------------------------------------------------

struct Stopped {};  // a channel / stop call said "the thread must stop"

using Script = std::function<void(SpuThread&)>;

class ScriptExecutor final : public spu::ISpuExecutor {
public:
    explicit ScriptExecutor(Script script) : script_(std::move(script)) {}
    void Run(SpuThread& t) override {
        try {
            script_(t);
        } catch (const Stopped&) {
        }
    }

private:
    Script script_;
};

void InstallScripts(Machine& m, std::vector<Script> scripts) {
    m.kernel->spu().SetExecutorFactory([scripts = std::move(scripts)](SpuThread& t) -> std::unique_ptr<spu::ISpuExecutor> {
        return std::make_unique<ScriptExecutor>(scripts.at(t.spu_num));
    });
}

u32 Rd(SpuThread& t, u32 ch) {
    u32 v = 0;
    if (!t.ReadChannel(ch, v)) throw Stopped{};
    return v;
}
void Wr(SpuThread& t, u32 ch, u32 v) {
    if (!t.WriteChannel(ch, v)) throw Stopped{};
}
void Dma(SpuThread& t, u32 cmd, u32 lsa, u32 ea, u32 size, u32 tag = 0) {
    Wr(t, spu::MFC_LSA, lsa);
    Wr(t, spu::MFC_EAH, 0);
    Wr(t, spu::MFC_EAL, ea);
    Wr(t, spu::MFC_Size, size);
    Wr(t, spu::MFC_TagID, tag);
    Wr(t, spu::MFC_Cmd, cmd);
}
void Stop(SpuThread& t, u32 code) {
    if (!t.StopAndSignal(code)) throw Stopped{};
}
void Exit(SpuThread& t, u32 status) {
    Wr(t, spu::SPU_WrOutMbox, status);
    t.StopAndSignal(spu::SYS_SPU_THREAD_STOP_THREAD_EXIT);
    throw Stopped{};
}
// The LS is shared with the PPU (sys_spu_thread_read_ls/write_ls) and with peer DMA: scripts
// touch it the way the executor must, with relaxed atomics.
u32 Ls32(const SpuThread& t, u32 lsa) {
    return ByteSwap(std::atomic_ref<u32>(*reinterpret_cast<u32*>(t.ctx.ls + lsa)).load(std::memory_order_relaxed));
}
void SetLs32(SpuThread& t, u32 lsa, u32 v) {
    std::atomic_ref<u32>(*reinterpret_cast<u32*>(t.ctx.ls + lsa)).store(ByteSwap(v), std::memory_order_relaxed);
}
u32 PreferredWord(const ppu::v128& r) { return u32{r.bytes[0]} << 24 | u32{r.bytes[1]} << 16 | u32{r.bytes[2]} << 8 | r.bytes[3]; }
u64 PreferredDoubleword(const ppu::v128& r) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | r.bytes[i];
    return v;
}

// ---------------------------------------------------------------------------------------
// PPU side: syscalls and HLE calls issued from the test thread
// ---------------------------------------------------------------------------------------

ppu::PpuThreadContext SysCtx(Machine& m, u32 number, std::initializer_list<u64> args) {
    ppu::PpuThreadContext c{};
    c.gpr[11] = number;
    int i = 3;
    for (u64 a : args) c.gpr[i++] = a;
    m.syscalls.Dispatch(c);
    return c;
}
s32 Sys(Machine& m, u32 number, std::initializer_list<u64> args) {
    return static_cast<s32>(SysCtx(m, number, args).gpr[3]);
}
ppu::PpuThreadContext Hle(Machine& m, const char* module, const char* name, std::initializer_list<u64> args) {
    const u32 index = m.hle->Bind(module, hle::ComputeFnid(name));
    const hle::HleFunction& f = m.hle->At(index);
    ppu::PpuThreadContext c{};
    int i = 3;
    for (u64 a : args) c.gpr[i++] = a;
    f.fn(c, m.memory, f);
    return c;
}

// Data layout (offsets from kData)
constexpr u32 kGroupAttr = 0x000, kGroupName = 0x040, kThreadAttr = 0x080, kThreadName = 0x0C0, kArgs = 0x100;
constexpr u32 kImage = 0x180, kOut = 0x200, kQueueAttr = 0x300, kElf = 0x400;
constexpr u32 kDmaSrc = 0x1000, kDmaDst = 0x1100, kAtomicLine = 0x1200, kWatchLine = 0x1280;
// Out slots
constexpr u32 kGroupId = kOut + 0x00, kPpuQueue = kOut + 0x0C;
constexpr u32 kSpuQueue = kOut + 0x10, kPortId = kOut + 0x14, kCause = kOut + 0x18, kStatus = kOut + 0x1C;
constexpr u32 kValue = kOut + 0x20;
// SPU program image: 0x40 bytes of code/data at LS 0x100, 0x40 bytes of bss after it.
constexpr u32 kEntry = 0x100, kPayload = 0x40, kBss = 0x40, kPayloadOffset = 0x80;

void WriteString(Machine& m, u32 at, const char* s) {
    m.memory.CopyIn(at, s, std::strlen(s) + 1);
}

// A minimal SPU ELF (ELF32 BE, EM_SPU, one PT_LOAD) at kData + kElf.
void BuildSpuElf(Machine& m) {
    const u32 e = kData + kElf;
    m.memory.Fill(e, 0, 0x100);
    const u8 ident[] = {0x7F, 'E', 'L', 'F', 1, 2, 1};
    m.memory.CopyIn(e, ident, sizeof(ident));
    m.memory.Write<u16>(e + 16, 2);   // ET_EXEC
    m.memory.Write<u16>(e + 18, 23);  // EM_SPU
    m.memory.Write<u32>(e + 20, 1);
    m.memory.Write<u32>(e + 24, kEntry);
    m.memory.Write<u32>(e + 28, 52);  // e_phoff
    m.memory.Write<u16>(e + 40, 52);
    m.memory.Write<u16>(e + 42, 32);
    m.memory.Write<u16>(e + 44, 1);
    const u32 ph = e + 52;
    m.memory.Write<u32>(ph + 0, 1);  // PT_LOAD
    m.memory.Write<u32>(ph + 4, kPayloadOffset);
    m.memory.Write<u32>(ph + 8, kEntry);
    m.memory.Write<u32>(ph + 16, kPayload);
    m.memory.Write<u32>(ph + 20, kPayload + kBss);
    for (u32 i = 0; i < kPayload; ++i) m.memory.Write<u8>(e + kPayloadOffset + i, static_cast<u8>(0x10 + i));
}

// sys_spu_initialize + image import (PROTECT) + attributes; returns false on failure.
void PrepareSpuEnvironment(Machine& m, hle::SysSpuHle& spu_hle) {
    spu_hle.Register(*m.hle);
    CHECK_EQ(Sys(m, 169, {6, 0}), CELL_OK);
    BuildSpuElf(m);
    WriteString(m, kData + kGroupName, "TestGroup");
    m.memory.Write<u32>(kData + kGroupAttr + 0, 10);
    m.memory.Write<u32>(kData + kGroupAttr + 4, kData + kGroupName);
    m.memory.Write<u32>(kData + kGroupAttr + 8, 0);
    m.memory.Write<u32>(kData + kGroupAttr + 12, 0);
    WriteString(m, kData + kThreadName, "TestSpu");
    m.memory.Write<u32>(kData + kThreadAttr + 0, kData + kThreadName);
    m.memory.Write<u32>(kData + kThreadAttr + 4, 8);
    m.memory.Write<u32>(kData + kThreadAttr + 8, 0);
    const auto c = Hle(m, "sysPrxForUser", "sys_spu_image_import", {kData + kImage, kData + kElf, spu::SYS_SPU_IMAGE_PROTECT});
    CHECK_EQ(static_cast<s32>(c.gpr[3]), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kData + kImage + 4), kEntry);
    CHECK_EQ(m.memory.Read<u32>(kData + kImage + 12), 2);  // COPY + FILL
}

u32 CreateGroup(Machine& m, u32 num) {
    CHECK_EQ(Sys(m, 170, {kData + kGroupId, num, 100, kData + kGroupAttr}), CELL_OK);
    return m.memory.Read<u32>(kData + kGroupId);
}

u32 InitThread(Machine& m, u32 group, u32 spu_num, u64 arg1) {
    m.memory.Write<u64>(kData + kArgs + 0, arg1);
    m.memory.Write<u64>(kData + kArgs + 8, arg1 + 1);
    m.memory.Write<u64>(kData + kArgs + 16, 0);
    m.memory.Write<u64>(kData + kArgs + 24, 0);
    const u32 out = kData + kValue;
    CHECK_EQ(Sys(m, 172, {out, group, spu_num, kData + kImage, kData + kThreadAttr, kData + kArgs}), CELL_OK);
    return m.memory.Read<u32>(out);
}

u32 CreateQueue(Machine& m, s32 type, u32 out) {
    m.memory.Write<u32>(kData + kQueueAttr + 0, lv2::SYS_SYNC_FIFO);
    m.memory.Write<u32>(kData + kQueueAttr + 4, static_cast<u32>(type));
    m.memory.Write<u64>(kData + kQueueAttr + 8, 0);
    CHECK_EQ(Sys(m, 128, {kData + out, kData + kQueueAttr, 0, 16}), CELL_OK);
    return m.memory.Read<u32>(kData + out);
}

s32 Join(Machine& m, u32 group, s32* cause, s32* status) {
    const s32 r = Sys(m, 178, {group, kData + kCause, kData + kStatus});
    *cause = static_cast<s32>(m.memory.Read<u32>(kData + kCause));
    *status = static_cast<s32>(m.memory.Read<u32>(kData + kStatus));
    return r;
}

// ---------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------

// Two SPU threads: image load, arguments, mailbox, DMA GET/PUT, atomic reservation, SPU->PPU
// event, SPU->SPU DMA and signal, PPU->SPU event (bound queue), thread exit + join.
void TestFullFlow() {
    std::printf("-- SPU group: image, mailbox, DMA, atomics, events, signals, exit/join\n");
    Watchdog dog("full SPU flow");
    Machine m;
    hle::SysSpuHle spu_hle(*m.kernel);
    PrepareSpuEnvironment(m, spu_hle);
    // PROTECT import: the image must not depend on the ELF bytes any more.
    for (u32 i = 0; i < kPayload; ++i) m.memory.Write<u8>(kData + kElf + kPayloadOffset + i, 0xEE);

    const u32 ppu_queue = CreateQueue(m, lv2::SYS_PPU_QUEUE, kPpuQueue);
    const u32 spu_queue = CreateQueue(m, lv2::SYS_SPU_QUEUE, kSpuQueue);
    const u32 group = CreateGroup(m, 2);
    const u32 t0 = InitThread(m, group, 0, 0x1111222233334444ull);
    CHECK_EQ(Sys(m, 173, {group}), CELL_ESTAT);  // thread 1 missing
    const u32 t1 = InitThread(m, group, 1, 0x5555666677778888ull);
    CHECK_EQ(Sys(m, 191, {t0, ppu_queue, lv2::SYS_SPU_THREAD_EVENT_USER, 5}), CELL_OK);
    CHECK_EQ(Sys(m, 191, {t0, ppu_queue, lv2::SYS_SPU_THREAD_EVENT_USER, 5}), CELL_EISCONN);
    CHECK_EQ(Sys(m, 193, {t1, spu_queue, 7}), CELL_OK);
    CHECK_EQ(Sys(m, 193, {t1, spu_queue, 8}), CELL_EBUSY);  // same queue twice
    CHECK_EQ(Sys(m, 134, {kData + kPortId, lv2::SYS_EVENT_PORT_LOCAL, 0}), CELL_OK);
    const u32 port = m.memory.Read<u32>(kData + kPortId);
    CHECK_EQ(Sys(m, 136, {port, spu_queue}), CELL_OK);

    for (u32 i = 0; i < 32; ++i) m.memory.Write<u32>(kData + kDmaSrc + i * 4, 1000 + i);
    m.memory.Fill(kData + kAtomicLine, 0, 128);
    const u32 peer_window = spu::kSpuThreadBaseLow + 1 * spu::kLsWindow;

    std::vector<Script> scripts(2);
    scripts[0] = [&](SpuThread& t) {
        SCHECK(t.ctx.pc == kEntry);
        SCHECK(PreferredWord(t.ctx.gpr[1]) == 0x3FFF0);
        SCHECK(PreferredDoubleword(t.ctx.gpr[3]) == 0x1111222233334444ull);
        SCHECK(PreferredDoubleword(t.ctx.gpr[4]) == 0x1111222233334445ull);
        bool image_ok = true;
        for (u32 i = 0; i < kPayload; ++i) image_ok &= t.ctx.ls[kEntry + i] == static_cast<u8>(0x10 + i);
        for (u32 i = 0; i < kBss; ++i) image_ok &= t.ctx.ls[kEntry + kPayload + i] == 0;
        SCHECK(image_ok);

        const u32 v = Rd(t, spu::SPU_RdInMbox);
        SCHECK(v == 5);
        Dma(t, spu::MFC_GET_CMD, 0x2000, kData + kDmaSrc, 128, 3);
        Wr(t, spu::MFC_WrTagMask, 1u << 3);
        Wr(t, spu::MFC_WrTagUpdate, spu::MFC_TAG_UPDATE_ALL);
        SCHECK(Rd(t, spu::MFC_RdTagStat) == 1u << 3);
        for (u32 i = 0; i < 32; ++i) SetLs32(t, 0x2000 + i * 4, Ls32(t, 0x2000 + i * 4) + v);
        Dma(t, spu::MFC_PUT_CMD, 0x2000, kData + kDmaDst, 128, 3);

        // sys_spu_thread_send_event(spup 5, 0x123, 0xCAFE)
        Wr(t, spu::SPU_WrOutMbox, 0xCAFE);
        Wr(t, spu::SPU_WrOutIntrMbox, 5u << 24 | 0x123);
        SCHECK(Rd(t, spu::SPU_RdInMbox) == static_cast<u32>(CELL_OK));
        // unconnected port -> ENOTCONN
        Wr(t, spu::SPU_WrOutMbox, 1);
        Wr(t, spu::SPU_WrOutIntrMbox, 9u << 24);
        SCHECK(Rd(t, spu::SPU_RdInMbox) == static_cast<u32>(CELL_ENOTCONN));

        // Atomic increment of a 128-byte line: GETLLAR / PUTLLC
        Dma(t, spu::MFC_GETLLAR_CMD, 0x3000, kData + kAtomicLine, 128);
        SCHECK(Rd(t, spu::MFC_RdAtomicStat) == spu::MFC_GETLLAR_SUCCESS);
        SetLs32(t, 0x3000, Ls32(t, 0x3000) + 1);
        Dma(t, spu::MFC_PUTLLC_CMD, 0x3000, kData + kAtomicLine, 128);
        SCHECK(Rd(t, spu::MFC_RdAtomicStat) == spu::MFC_PUTLLC_SUCCESS);
        // A second PUTLLC without a new reservation must fail.
        Dma(t, spu::MFC_PUTLLC_CMD, 0x3000, kData + kAtomicLine, 128);
        SCHECK(Rd(t, spu::MFC_RdAtomicStat) == spu::MFC_PUTLLC_FAILURE);

        // SPU -> SPU: DMA into thread 1's LS, then signal it through SNR1.
        for (u32 i = 0; i < 4; ++i) SetLs32(t, 0x3100 + i * 4, 0xA0A0A0A0 + i);
        Dma(t, spu::MFC_PUT_CMD, 0x3100, peer_window + 0x1000, 16);
        SetLs32(t, 0x320C, 0x77);
        Dma(t, spu::MFC_SNDSIG_CMD, 0x320C, peer_window + 0x5400C, 4);
        Exit(t, 0x42);
    };
    scripts[1] = [&](SpuThread& t) {
        SCHECK(PreferredDoubleword(t.ctx.gpr[3]) == 0x5555666677778888ull);
        SCHECK(Rd(t, spu::SPU_RdSigNotify1) == 0x77);
        bool peer_ok = true;
        for (u32 i = 0; i < 4; ++i) peer_ok &= Ls32(t, 0x1000 + i * 4) == 0xA0A0A0A0 + i;
        SCHECK(peer_ok);
        // sys_spu_thread_receive_event(spuq 7)
        Wr(t, spu::SPU_WrOutMbox, 7);
        Stop(t, spu::SYS_SPU_THREAD_STOP_RECEIVE_EVENT);
        SCHECK(t.ChannelCount(spu::SPU_RdInMbox) == 4);
        SCHECK(Rd(t, spu::SPU_RdInMbox) == static_cast<u32>(CELL_OK));
        SCHECK(Rd(t, spu::SPU_RdInMbox) == 0xA);
        SCHECK(Rd(t, spu::SPU_RdInMbox) == 0xB);
        SCHECK(Rd(t, spu::SPU_RdInMbox) == 0xC);
        // try-receive on the now empty queue -> EBUSY
        Wr(t, spu::SPU_WrOutMbox, 7);
        Stop(t, spu::SYS_SPU_THREAD_STOP_TRY_RECEIVE_EVENT);
        SCHECK(Rd(t, spu::SPU_RdInMbox) == static_cast<u32>(CELL_EBUSY));
        Exit(t, 0x43);
    };
    InstallScripts(m, std::move(scripts));

    CHECK_EQ(Sys(m, 190, {t0, 5}), CELL_ESTAT);  // not running yet
    CHECK_EQ(Sys(m, 173, {group}), CELL_OK);
    CHECK_EQ(Sys(m, 173, {group}), CELL_ESTAT);  // already running
    CHECK_EQ(Sys(m, 190, {t0, 5}), CELL_OK);

    const auto ev = SysCtx(m, 130, {ppu_queue, 0, 0});
    CHECK_EQ(static_cast<s32>(ev.gpr[3]), CELL_OK);
    CHECK_EQ(ev.gpr[4], spu::SYS_SPU_THREAD_EVENT_USER_KEY);
    CHECK_EQ(ev.gpr[5], t0);
    CHECK_EQ(ev.gpr[6], (u64{5} << 32) | 0x123);
    CHECK_EQ(ev.gpr[7], 0xCAFE);
    CHECK_EQ(Sys(m, 138, {port, 0xA, 0xB, 0xC}), CELL_OK);

    s32 cause = 0, status = 0;
    CHECK_EQ(Join(m, group, &cause, &status), CELL_OK);
    CHECK_EQ(cause, spu::SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT);
    CHECK_EQ(Sys(m, 165, {t0, kData + kValue}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kData + kValue), 0x42);
    CHECK_EQ(Sys(m, 165, {t1, kData + kValue}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kData + kValue), 0x43);
    bool dma_ok = true;
    for (u32 i = 0; i < 32; ++i) dma_ok &= m.memory.Read<u32>(kData + kDmaDst + i * 4) == 1005 + i;
    CHECK(dma_ok);
    CHECK_EQ(m.memory.Read<u32>(kData + kAtomicLine), 1);
    CHECK(!m.kernel->is_terminating());
    CHECK_EQ(Sys(m, 171, {group}), CELL_OK);
    CHECK(!m.kernel->spu().FindThread(t0));
    CHECK_EQ(Sys(m, 171, {group}), CELL_ESRCH);
}

// Group exit, restart of the same group, terminate while threads are blocked.
void TestGroupExitRestartTerminate() {
    std::printf("-- SPU group: group exit, restart, terminate blocked threads\n");
    Watchdog dog("group exit / terminate");
    Machine m;
    hle::SysSpuHle spu_hle(*m.kernel);
    PrepareSpuEnvironment(m, spu_hle);
    const u32 group = CreateGroup(m, 2);
    const u32 t0 = InitThread(m, group, 0, 0);
    InitThread(m, group, 1, 0);
    std::atomic<int> runs{0};
    std::atomic<int> blocked_stops{0};
    std::vector<Script> scripts(2);
    scripts[0] = [&](SpuThread& t) {
        u32 v = 0;
        if (!t.ReadChannel(spu::SPU_RdInMbox, v)) blocked_stops.fetch_add(1);  // woken by the group stop
    };
    scripts[1] = [&](SpuThread& t) {
        if (runs.fetch_add(1) == 0) {
            Wr(t, spu::SPU_WrOutMbox, 9);
            SCHECK(!t.StopAndSignal(spu::SYS_SPU_THREAD_STOP_GROUP_EXIT));
            return;
        }
        u32 v = 0;
        if (!t.ReadChannel(spu::SPU_RdInMbox, v)) blocked_stops.fetch_add(1);
    };
    InstallScripts(m, std::move(scripts));

    s32 cause = 0, status = 0;
    CHECK_EQ(Join(m, group, &cause, &status), CELL_ESTAT);  // never started
    CHECK_EQ(Sys(m, 173, {group}), CELL_OK);
    CHECK_EQ(Join(m, group, &cause, &status), CELL_OK);
    CHECK_EQ(cause, spu::SYS_SPU_THREAD_GROUP_JOIN_GROUP_EXIT);
    CHECK_EQ(status, 9);
    CHECK_EQ(blocked_stops.load(), 1);

    CHECK_EQ(Sys(m, 173, {group}), CELL_OK);  // same group, second run
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(Sys(m, 171, {group}), CELL_EBUSY);  // destroy while running
    CHECK_EQ(Sys(m, 177, {group, 77}), CELL_OK);
    CHECK_EQ(Join(m, group, &cause, &status), CELL_OK);
    CHECK_EQ(cause, spu::SYS_SPU_THREAD_GROUP_JOIN_TERMINATED);
    CHECK_EQ(status, 77);
    CHECK_EQ(blocked_stops.load(), 3);
    CHECK_EQ(Sys(m, 165, {t0, kData + kValue}), CELL_ESTAT);  // not a thread exit
    CHECK_EQ(Sys(m, 177, {group, 1}), CELL_ESTAT);           // not running
    CHECK(!m.kernel->is_terminating());
    CHECK_EQ(Sys(m, 171, {group}), CELL_OK);
}

// Event flag bit (out-interrupt mailbox code 128), decrementer event, signal OR mode,
// reservation-lost event, LS read/write from the PPU.
void TestEventsAndSignals() {
    std::printf("-- SPU events: event flag bit, decrementer, SNR OR mode, LR, read/write_ls\n");
    Watchdog dog("SPU events");
    Machine m;
    hle::SysSpuHle spu_hle(*m.kernel);
    PrepareSpuEnvironment(m, spu_hle);
    auto flag = std::make_shared<lv2::Lv2EventFlag>();
    const u32 flag_id = m.kernel->objects().Add(lv2::kIdEventFlag, flag);
    const u32 group = CreateGroup(m, 1);
    const u32 t0 = InitThread(m, group, 0, 0);
    CHECK_EQ(Sys(m, 187, {t0, 4}), CELL_EINVAL);
    CHECK_EQ(Sys(m, 187, {t0, 1}), CELL_OK);  // SNR1 in OR mode
    CHECK_EQ(Sys(m, 188, {t0, kData + kValue}), CELL_OK);
    CHECK_EQ(m.memory.Read<u64>(kData + kValue), 1);
    CHECK_EQ(Sys(m, 181, {t0, 0x100, 1, 4}), CELL_ESTAT);  // write_ls before start

    std::vector<Script> scripts(1);
    scripts[0] = [&](SpuThread& t) {
        SCHECK(Rd(t, spu::SPU_RdInMbox) == 1);  // "go": both signals were written
        SCHECK(Rd(t, spu::SPU_RdSigNotify1) == (0x10 | 0x02));
        // sys_event_flag_set_bit(flag, 3)
        Wr(t, spu::SPU_WrOutMbox, flag_id);
        Wr(t, spu::SPU_WrOutIntrMbox, 128u << 24 | 3);
        SCHECK(Rd(t, spu::SPU_RdInMbox) == static_cast<u32>(CELL_OK));
        // ... and an unknown flag id -> ESRCH
        Wr(t, spu::SPU_WrOutMbox, 0x12345678);
        Wr(t, spu::SPU_WrOutIntrMbox, 128u << 24 | 1);
        SCHECK(Rd(t, spu::SPU_RdInMbox) == static_cast<u32>(CELL_ESRCH));
        // sys_event_flag_set_bit_impatient(flag, 5): no reply; tells the PPU the reply slot is free
        Wr(t, spu::SPU_WrOutMbox, flag_id);
        Wr(t, spu::SPU_WrOutIntrMbox, 192u << 24 | 5);
        // Decrementer: ~10 us, then wait for SPU_EVENT_TM
        Wr(t, spu::SPU_WrEventMask, spu::SPU_EVENT_TM);
        Wr(t, spu::SPU_WrDec, 800);
        SCHECK(Rd(t, spu::SPU_RdEventStat) & spu::SPU_EVENT_TM);
        Wr(t, spu::SPU_WrEventAck, spu::SPU_EVENT_TM);
        SCHECK(t.ChannelCount(spu::SPU_RdEventStat) == 0);
        // Reservation lost: reserve a line, change it from elsewhere, wait for SPU_EVENT_LR
        Dma(t, spu::MFC_GETLLAR_CMD, 0x3000, kData + kWatchLine, 128);
        SCHECK(Rd(t, spu::MFC_RdAtomicStat) == spu::MFC_GETLLAR_SUCCESS);
        Wr(t, spu::SPU_WrEventMask, spu::SPU_EVENT_LR);
        std::thread writer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            std::atomic_ref<u32>(*reinterpret_cast<u32*>(m.memory.shadow() + kData + kWatchLine))
                .store(0xFFFFFFFF, std::memory_order_relaxed);
        });
        SCHECK(Rd(t, spu::SPU_RdEventStat) & spu::SPU_EVENT_LR);
        writer.join();
        Wr(t, spu::SPU_WrEventAck, spu::SPU_EVENT_LR);
        // Value written by the PPU with sys_spu_thread_write_ls
        SCHECK(Rd(t, spu::SPU_RdInMbox) == 2);
        SCHECK(Ls32(t, 0x8000) == 0xDEADBEEF);
        SetLs32(t, 0x8004, 0x0BADF00D);  // the PPU polls it with sys_spu_thread_read_ls
        SCHECK(Rd(t, spu::SPU_RdInMbox) == 3);
        Exit(t, 0);
    };
    InstallScripts(m, std::move(scripts));
    CHECK_EQ(Sys(m, 173, {group}), CELL_OK);
    CHECK_EQ(Sys(m, 184, {t0, 0, 0x10}), CELL_OK);
    CHECK_EQ(Sys(m, 184, {t0, 0, 0x02}), CELL_OK);
    CHECK_EQ(Sys(m, 184, {t0, 2, 0x02}), CELL_EINVAL);
    CHECK_EQ(Sys(m, 190, {t0, 1}), CELL_OK);
    // The inbound mailbox doubles as the reply slot of the SPU's lv2 requests: a PPU write
    // while a request is pending makes lv2 answer CELL_EBUSY. Wait for the SPU's "done" bit.
    for (bool done = false; !done; std::this_thread::yield()) {
        std::lock_guard lock(flag->m);
        done = flag->pattern & (1u << 5);
    }
    CHECK_EQ(Sys(m, 181, {t0, 0x8000, 0xDEADBEEF, 4}), CELL_OK);
    CHECK_EQ(Sys(m, 181, {t0, 0x8002, 0, 4}), CELL_EINVAL);  // misaligned
    CHECK_EQ(Sys(m, 181, {t0, 0x40000, 0, 1}), CELL_EINVAL); // outside the LS
    CHECK_EQ(Sys(m, 190, {t0, 2}), CELL_OK);
    // Poll until the SPU wrote LS 0x8004.
    for (int i = 0; i < 2000; ++i) {
        CHECK_EQ(Sys(m, 182, {t0, 0x8004, kData + kValue, 4}), CELL_OK);
        if (m.memory.Read<u64>(kData + kValue) == 0x0BADF00D) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_EQ(m.memory.Read<u64>(kData + kValue), 0x0BADF00D);
    CHECK_EQ(Sys(m, 190, {t0, 3}), CELL_OK);
    s32 cause = 0, status = 0;
    CHECK_EQ(Join(m, group, &cause, &status), CELL_OK);
    CHECK_EQ(cause, spu::SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT);
    {
        std::lock_guard lock(flag->m);
        CHECK_EQ(flag->pattern, (1u << 3) | (1u << 5));
    }
    CHECK(!m.kernel->is_terminating());
}

// Faults end the process with a report: bad DMA, invalid channel, no executor installed.
void RunFaultCase(const char* what, Script script, const char* expected) {
    Watchdog dog(what);
    Machine m;
    hle::SysSpuHle spu_hle(*m.kernel);
    PrepareSpuEnvironment(m, spu_hle);
    const u32 group = CreateGroup(m, 1);
    InitThread(m, group, 0, 0);
    if (script) InstallScripts(m, {script});
    CHECK_EQ(Sys(m, 173, {group}), CELL_OK);
    s32 cause = 0, status = 0;
    const s32 r = Join(m, group, &cause, &status);
    CHECK(r == CELL_OK || r == CELL_ECANCELED);
    CHECK(m.kernel->is_terminating());
    const lv2::ProcessEnd end = m.kernel->process_end();
    CHECK(end.kind == ppu::kExitAbort);
    CHECK(end.message.find(expected) != std::string::npos);
    if (end.message.find(expected) == std::string::npos) std::fprintf(stderr, "  message: %s\n", end.message.c_str());
    const auto lines = m.kernel->spu().Describe();
    CHECK(lines.size() == 2 && lines[1].find("fault") != std::string::npos);
}

void TestFaults() {
    std::printf("-- SPU faults: bad DMA, misaligned DMA, invalid channel, missing executor\n");
    RunFaultCase("unmapped DMA", [](SpuThread& t) { Dma(t, spu::MFC_GET_CMD, 0x100, 0x08000000, 16); },
                 "MFC GET: EA 0x08000000+0x10 is not accessible");
    RunFaultCase("misaligned DMA", [](SpuThread& t) { Dma(t, spu::MFC_PUT_CMD, 0x104, kData + 0x2000, 32); },
                 "MFC PUT: invalid transfer");
    RunFaultCase("invalid channel", [](SpuThread& t) { (void)Rd(t, 31); }, "rdch on invalid channel 31");
    RunFaultCase("missing stop", [](SpuThread&) {}, "returned without stop");
    RunFaultCase("no executor", nullptr, "no SPU executor is installed");
}

void TestValidation() {
    std::printf("-- SPU syscall validation\n");
    Machine m;
    hle::SysSpuHle spu_hle(*m.kernel);
    PrepareSpuEnvironment(m, spu_hle);
    const u32 out = kData + kGroupId, attr = kData + kGroupAttr;
    CHECK_EQ(Sys(m, 170, {out, 0, 100, attr}), CELL_EINVAL);
    CHECK_EQ(Sys(m, 170, {out, 7, 100, attr}), CELL_EINVAL);
    CHECK_EQ(Sys(m, 170, {out, 1, 5, attr}), CELL_EINVAL);   // priority below 16
    CHECK_EQ(Sys(m, 170, {out, 1, 100, 0}), CELL_EFAULT);    // no attribute
    const u32 g1 = CreateGroup(m, 4);
    CHECK_EQ(Sys(m, 170, {out, 3, 100, attr}), CELL_EBUSY);  // 4 + 3 > 6 SPUs
    const u32 g2 = CreateGroup(m, 2);
    CHECK(g1 != g2);
    CHECK_EQ(Sys(m, 172, {kData + kValue, g1, 4, kData + kImage, kData + kThreadAttr, kData + kArgs}), CELL_EINVAL);
    CHECK_EQ(Sys(m, 172, {kData + kValue, 0x12345, 0, kData + kImage, kData + kThreadAttr, kData + kArgs}), CELL_ESRCH);
    const u32 t = InitThread(m, g2, 0, 0);
    CHECK_EQ(Sys(m, 172, {kData + kValue, g2, 0, kData + kImage, kData + kThreadAttr, kData + kArgs}), CELL_EBUSY);
    // LS window: 256 KiB accessible, the rest of the 1 MiB window a guard
    const auto thread = m.kernel->spu().FindThread(t);
    CHECK(thread && thread->ctx.ls_ea >= spu::kSpuThreadBaseLow && thread->ctx.ls_ea % spu::kLsWindow == 0);
    CHECK(m.memory.Check(thread->ctx.ls_ea, spu::kLsSize, vm::kPageWritable));
    CHECK(!m.memory.Check(thread->ctx.ls_ea + spu::kLsSize, 4, vm::kPageReadable));
    const u32 ppu_queue = CreateQueue(m, lv2::SYS_PPU_QUEUE, kPpuQueue);
    const u32 spu_queue = CreateQueue(m, lv2::SYS_SPU_QUEUE, kSpuQueue);
    CHECK_EQ(Sys(m, 191, {t, spu_queue, 1, 0}), CELL_EINVAL);   // must be a PPU queue
    CHECK_EQ(Sys(m, 191, {t, ppu_queue, 2, 0}), CELL_EINVAL);   // event type
    CHECK_EQ(Sys(m, 191, {t, ppu_queue, 1, 64}), CELL_EINVAL);  // port range
    CHECK_EQ(Sys(m, 192, {t, 1, 3}), CELL_ENOTCONN);
    CHECK_EQ(Sys(m, 193, {t, ppu_queue, 1}), CELL_EINVAL);      // must be an SPU queue
    CHECK_EQ(Sys(m, 194, {t, 1}), CELL_ESRCH);
    CHECK_EQ(Sys(m, 193, {t, spu_queue, 1}), CELL_OK);
    CHECK_EQ(Sys(m, 194, {t, 1}), CELL_OK);
    CHECK_EQ(Sys(m, 185, {g2, ppu_queue, 2}), CELL_OK);
    CHECK_EQ(Sys(m, 185, {g2, ppu_queue, 2}), CELL_EBUSY);
    CHECK_EQ(Sys(m, 186, {g2, 2}), CELL_OK);
    CHECK_EQ(Sys(m, 186, {g2, 3}), CELL_EINVAL);
    CHECK_EQ(Sys(m, 179, {g2, 300}), CELL_EINVAL);
    CHECK_EQ(Sys(m, 179, {g2, 200}), CELL_OK);
    CHECK_EQ(Sys(m, 180, {g2, kData + kValue}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kData + kValue), 200);
    CHECK_EQ(Sys(m, 171, {g1}), CELL_OK);
    CHECK_EQ(Sys(m, 171, {g2}), CELL_OK);
    CHECK(m.memory.Check(thread->ctx.ls_ea, 4, vm::kPageReadable) == false);  // LS released
    CHECK_EQ(Sys(m, 170, {out, 6, 100, attr}), CELL_OK);  // all 6 SPUs free again
}

void TestImageHle() {
    std::printf("-- sys_spu_image_import / close, sys_spu_elf_get_*, raw SPU abort\n");
    Machine m;
    hle::SysSpuHle spu_hle(*m.kernel);
    PrepareSpuEnvironment(m, spu_hle);
    auto c = Hle(m, "sysPrxForUser", "sys_spu_elf_get_information", {kData + kElf, kData + kValue, kData + kValue + 4});
    CHECK_EQ(static_cast<s32>(c.gpr[3]), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kData + kValue), kEntry);
    CHECK_EQ(m.memory.Read<u32>(kData + kValue + 4), 2);
    c = Hle(m, "sysPrxForUser", "sys_spu_elf_get_segments", {kData + kElf, kData + 0x3000, 2});
    CHECK_EQ(static_cast<s32>(c.gpr[3]), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kData + 0x3000), spu::SYS_SPU_SEGMENT_TYPE_COPY);
    CHECK_EQ(m.memory.Read<u32>(kData + 0x3000 + 8), kPayload);
    CHECK_EQ(m.memory.Read<u32>(kData + 0x3000 + 12), kData + kElf + kPayloadOffset);
    CHECK_EQ(m.memory.Read<u32>(kData + 0x3010), spu::SYS_SPU_SEGMENT_TYPE_FILL);
    CHECK_EQ(m.memory.Read<u32>(kData + 0x3010 + 4), kEntry + kPayload);
    // DIRECT import references the ELF in place.
    c = Hle(m, "sysPrxForUser", "sys_spu_image_import", {kData + 0x3100, kData + kElf, spu::SYS_SPU_IMAGE_DIRECT});
    CHECK_EQ(static_cast<s32>(c.gpr[3]), CELL_OK);
    const u32 segs = m.memory.Read<u32>(kData + 0x3100 + 8);
    CHECK_EQ(m.memory.Read<u32>(segs + 12), kData + kElf + kPayloadOffset);
    c = Hle(m, "sysPrxForUser", "sys_spu_image_close", {kData + 0x3100});
    CHECK_EQ(static_cast<s32>(c.gpr[3]), CELL_OK);
    CHECK(!m.memory.Check(segs, 4, vm::kPageReadable));
    // Not an SPU ELF
    m.memory.Write<u16>(kData + kElf + 18, 21);  // EM_PPC64
    c = Hle(m, "sysPrxForUser", "sys_spu_image_import", {kData + 0x3100, kData + kElf, spu::SYS_SPU_IMAGE_PROTECT});
    CHECK_EQ(static_cast<s32>(c.gpr[3]), CELL_ENOEXEC);
    c = Hle(m, "sysPrxForUser", "sys_raw_spu_load", {0, 0, 0});
    CHECK(c.exit_requested && c.exit_kind == ppu::kExitAbort && c.exit_message);
}

void TestCellSpurs() {
    std::printf("-- cellSpurs: initialize bookkeeping, work submission stops with a report\n");
    Machine m;
    hle::CellSpurs spurs;
    spurs.Register(*m.hle);
    const u32 obj = kData + 0x4000, attr = kData + 0x5000;  // 128- and 8-aligned
    auto c = Hle(m, "cellSpurs", "cellSpursInitialize", {obj + 8, 5, 200, 1000, 0});
    CHECK_EQ(static_cast<s32>(c.gpr[3]), hle::CELL_SPURS_CORE_ERROR_ALIGN);
    c = Hle(m, "cellSpurs", "cellSpursInitialize", {obj, 7, 200, 1000, 0});
    CHECK_EQ(static_cast<s32>(c.gpr[3]), hle::CELL_SPURS_CORE_ERROR_INVAL);
    c = Hle(m, "cellSpurs", "cellSpursAttributeInitialize", {attr, 5, 200, 1000, 1});
    CHECK_EQ(static_cast<s32>(c.gpr[3]), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(attr + 8), 5);
    c = Hle(m, "cellSpurs", "cellSpursAttributeSetNamePrefix", {attr, kData + kGroupName, 16});
    CHECK_EQ(static_cast<s32>(c.gpr[3]), hle::CELL_SPURS_CORE_ERROR_INVAL);
    c = Hle(m, "cellSpurs", "cellSpursInitializeWithAttribute", {obj, attr});
    CHECK_EQ(static_cast<s32>(c.gpr[3]), CELL_OK);
    CHECK(spurs.IsInitialized(obj));
    c = Hle(m, "cellSpurs", "cellSpursInitializeWithAttribute", {obj, attr});
    CHECK_EQ(static_cast<s32>(c.gpr[3]), hle::CELL_SPURS_CORE_ERROR_STAT);
    c = Hle(m, "cellSpurs", "cellSpursAttachLv2EventQueue", {obj, 0x8D000001, kData + kValue, 1});
    CHECK_EQ(static_cast<s32>(c.gpr[3]), CELL_OK);
    CHECK(m.memory.Read<u8>(kData + kValue) >= 0x10);
    c = Hle(m, "cellSpurs", "cellSpursCreateTaskset", {obj, kData + 0x6000, 0, 0, 0});
    CHECK(c.exit_requested && c.exit_kind == ppu::kExitAbort && c.exit_message);
    c = Hle(m, "cellSpurs", "cellSpursFinalize", {obj});
    CHECK_EQ(static_cast<s32>(c.gpr[3]), CELL_OK);
    CHECK(!spurs.IsInitialized(obj));
}

// Kernel::Shutdown with SPU threads still blocked must stop and join them.
void TestShutdownWithRunningGroup() {
    std::printf("-- process end with SPU threads blocked in channel and queue waits\n");
    Watchdog dog("shutdown with running SPU group");
    Machine m;
    hle::SysSpuHle spu_hle(*m.kernel);
    PrepareSpuEnvironment(m, spu_hle);
    const u32 spu_queue = CreateQueue(m, lv2::SYS_SPU_QUEUE, kSpuQueue);
    const u32 group = CreateGroup(m, 2);
    InitThread(m, group, 0, 0);
    const u32 t1 = InitThread(m, group, 1, 0);
    CHECK_EQ(Sys(m, 193, {t1, spu_queue, 3}), CELL_OK);
    std::atomic<int> waiting{0};
    std::vector<Script> scripts(2);
    scripts[0] = [&](SpuThread& t) {
        waiting.fetch_add(1);
        (void)Rd(t, spu::SPU_RdSigNotify2);
    };
    scripts[1] = [&](SpuThread& t) {
        waiting.fetch_add(1);
        Wr(t, spu::SPU_WrOutMbox, 3);
        Stop(t, spu::SYS_SPU_THREAD_STOP_RECEIVE_EVENT);
    };
    InstallScripts(m, std::move(scripts));
    CHECK_EQ(Sys(m, 173, {group}), CELL_OK);
    while (waiting.load() < 2) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    m.kernel->Shutdown();
    CHECK(m.kernel->is_terminating());
    const auto lines = m.kernel->spu().Describe();
    CHECK(lines.size() == 3 && lines[1].find("terminated") != std::string::npos &&
          lines[2].find("terminated") != std::string::npos);
}

}  // namespace

int main() {
    log::Threshold() = log::Level::Error;
    TestFullFlow();
    TestGroupExitRestartTerminate();
    TestEventsAndSignals();
    TestFaults();
    TestValidation();
    TestImageHle();
    TestCellSpurs();
    TestShutdownWithRunningGroup();
    const int failures = g_failures + g_spu_failures.load();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all SPU tests passed\n");
    return 0;
}
