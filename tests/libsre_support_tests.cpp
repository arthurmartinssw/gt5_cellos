// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/libsre_support_tests.cpp
//
// Phase 7.1: what the real libsre.sprx needs from us beyond the PRX loader.
//   * the NIDs the user's log could not name, checked against their SHA-1 FNIDs;
//   * syscall 14 sys_process_is_spu_lock_line_reservation_address (RPCS3 semantics);
//   * syscall 30 / sys_process_get_paramsfo and the PARAM.SFO reader;
//   * sysPrxForUser _sys_vprintf / _sys_vsnprintf / _sys_vsprintf (va_list) and _sys_strncat;
//   * _sys_spu_printf_*: callbacks stored by initialize and called by attach/detach;
//   * cellLibprof's cellUserTrace* accepted;
//   * the stall report: a PPU thread blocked in sys_event_queue_receive is reported with the
//     syscall, the caller and the queue's waiter count, and the monitor fires once.
#include <chrono>
#include <cstring>
#include <thread>

#include "hle/cell_libprof.h"
#include "hle/fnid.h"
#include "hle/sys_spu_hle.h"
#include "loader/param_sfo.h"
#include "lv2/stall_monitor.h"
#include "lv2/sys_event.h"
#include "ppu_asm.h"
#include "test_machine.h"

using namespace cell;

namespace {

s32 Sys(Machine& m, u32 n, std::initializer_list<u64> args) {
    ppu::PpuThreadContext c{};
    c.gpr[11] = n;
    int r = 3;
    for (u64 a : args) c.gpr[r++] = a;
    m.syscalls.Dispatch(c);
    return static_cast<s32>(c.gpr[3]);
}

// Calls an HLE function by (module, name) on the test thread; returns r3.
u64 Hle(Machine& m, const char* module, const char* name, std::initializer_list<u64> args) {
    const u32 index = m.hle->Bind(module, hle::ComputeFnid(name));
    const hle::HleFunction& f = m.hle->At(index);
    ppu::PpuThreadContext c{};
    int r = 3;
    for (u64 a : args) c.gpr[r++] = a;
    f.fn(c, m.memory, f);
    return c.gpr[3];
}

void PutString(Machine& m, u32 at, const char* s) { m.memory.CopyIn(at, s, std::strlen(s) + 1); }
std::string GetString(Machine& m, u32 at) { return m.memory.ReadCString(at, 4096).value_or("<bad>"); }

void TestNids() {
    std::printf("-- NIDs from the libsre import log, identified by FNID\n");
    CHECK_EQ(hle::ComputeFnid("_sys_vsnprintf"), 0x0618936Bu);
    CHECK_EQ(hle::ComputeFnid("_sys_spu_printf_detach_group"), 0x5FDFB2FEu);
    CHECK_EQ(hle::ComputeFnid("_sys_strncat"), 0x996F7CF8u);
    CHECK_EQ(hle::ComputeFnid("_sys_spu_printf_attach_group"), 0xDD0C1E09u);
    CHECK_EQ(hle::ComputeFnid("sys_process_get_paramsfo"), 0xE75C40F2u);
    CHECK_EQ(hle::ComputeFnid("_sys_vprintf"), 0xFA7F693Du);
    CHECK_EQ(hle::ComputeFnid("cellUserTraceRegister"), 0x05893E7Cu);
    CHECK_EQ(hle::ComputeFnid("cellUserTraceUnregister"), 0x6D045C2Eu);
}

void TestLockLineSyscall() {
    std::printf("-- syscall 14 sys_process_is_spu_lock_line_reservation_address\n");
    Machine m;
    constexpr u64 kRaw = lv2::SYS_MEMORY_ACCESS_RIGHT_RAW_SPU, kThr = lv2::SYS_MEMORY_ACCESS_RIGHT_SPU_THR;
    CHECK_EQ(Sys(m, 14, {kData, kThr}), CELL_OK);              // main
    CHECK_EQ(Sys(m, 14, {0x20000000, kThr | kRaw}), CELL_OK);  // user64k
    CHECK_EQ(Sys(m, 14, {0x30000080, kThr}), CELL_OK);         // user1m
    CHECK_EQ(Sys(m, 14, {0xC0000000, kRaw}), CELL_OK);         // RSX local
    CHECK_EQ(Sys(m, 14, {0xE0000000, kRaw}), CELL_OK);         // raw SPU
    CHECK_EQ(Sys(m, 14, {0xF0000000, kThr}), CELL_OK);         // SPU thread area: threads yes
    CHECK_EQ(Sys(m, 14, {0xF0000000, kRaw}), CELL_EPERM);      //                  raw SPUs no
    CHECK_EQ(Sys(m, 14, {0xD0000000, kThr}), CELL_EPERM);      // stacks never
    CHECK_EQ(Sys(m, 14, {kData, 0}), CELL_EINVAL);             // no flag
    CHECK_EQ(Sys(m, 14, {kData, 4}), CELL_EINVAL);             // unknown flag
    CHECK_EQ(Sys(m, 14, {0x50000000, kThr}), CELL_EINVAL);     // mmapper, nothing mapped
    CHECK(m.memory.AllocateFixed(vm::Area::Mmapper, 0x50000000, 0x10000000, vm::kProtReadWrite));
    CHECK_EQ(Sys(m, 14, {0x50000080, kThr}), CELL_OK);
}

std::vector<u8> BuildSfo() {
    std::vector<u8> d(72 + 20, 0);
    auto le16 = [&](std::size_t at, u16 v) { d[at] = static_cast<u8>(v); d[at + 1] = static_cast<u8>(v >> 8); };
    auto le32 = [&](std::size_t at, u32 v) { for (int i = 0; i < 4; ++i) d[at + i] = static_cast<u8>(v >> (8 * i)); };
    d[1] = 'P'; d[2] = 'S'; d[3] = 'F';
    le32(4, 0x0101);
    le32(8, 52);   // key table
    le32(12, 72);  // data table
    le32(16, 2);
    le16(20, 0); le16(22, 0x0204); le32(24, 10); le32(28, 16); le32(32, 0);   // TITLE_ID
    le16(36, 9); le16(38, 0x0404); le32(40, 4); le32(44, 4); le32(48, 16);    // ATTRIBUTE
    std::memcpy(d.data() + 52, "TITLE_ID\0ATTRIBUTE", 19);
    std::memcpy(d.data() + 72, "BLUS30109", 9);
    le32(72 + 16, 0x20);
    return d;
}

void TestParamSfo() {
    std::printf("-- PARAM.SFO reader, syscall 30 and sys_process_get_paramsfo\n");
    const auto sfo = loader::ParseParamSfo(BuildSfo());
    CHECK(sfo.has_value());
    if (sfo) {
        CHECK(sfo->at("TITLE_ID").text == "BLUS30109");
        CHECK_EQ(sfo->at("ATTRIBUTE").integer, 0x20);
    }
    auto bad = BuildSfo();
    bad[1] = 'X';
    CHECK(!loader::ParseParamSfo(bad).has_value());
    bad = BuildSfo();
    bad[16] = 200;  // entry count past the end
    CHECK(!loader::ParseParamSfo(bad).has_value());

    Machine m;
    const u32 buf = kData + 0x100;
    CHECK_EQ(Sys(m, 30, {buf}), CELL_ENOENT);
    CHECK_EQ(static_cast<s32>(Hle(m, "sysPrxForUser", "sys_process_get_paramsfo", {buf})), CELL_ENOENT);
    m.process.title_id = "BLUS30109";
    m.memory.Fill(buf, 0xCC, 0x40);
    CHECK_EQ(Sys(m, 30, {buf}), CELL_OK);
    CHECK_EQ(m.memory.Read<u8>(buf), 0);
    CHECK(GetString(m, buf + 1) == "BLUS30109");
    CHECK_EQ(m.memory.Read<u8>(buf + 0x3F), 0);
    m.memory.Fill(buf, 0xCC, 0x40);
    CHECK_EQ(static_cast<s32>(Hle(m, "sysPrxForUser", "sys_process_get_paramsfo", {buf})), CELL_OK);
    CHECK(GetString(m, buf + 1) == "BLUS30109");
    CHECK_EQ(Sys(m, 30, {0xB0000000}), CELL_EFAULT);  // unmapped
}

void TestStringHle() {
    std::printf("-- _sys_vprintf / _sys_vsnprintf / _sys_vsprintf (va_list) and _sys_strncat\n");
    Machine m;
    const u32 fmt = kData + 0x100, str = kData + 0x180, va = kData + 0x200, dst = kData + 0x300;
    PutString(m, fmt, "%d-%s-%x");
    PutString(m, str, "abc");
    m.memory.Write<u64>(va + 0, static_cast<u64>(-42));  // 8-byte slots, as the PPU ABI spills them
    m.memory.Write<u64>(va + 8, str);
    m.memory.Write<u64>(va + 16, 0xBEEF);
    CHECK_EQ(Hle(m, "sysPrxForUser", "_sys_vsprintf", {dst, fmt, va}), 12);
    CHECK(GetString(m, dst) == "-42-abc-beef");
    m.memory.Fill(dst, 0xCC, 32);
    CHECK_EQ(Hle(m, "sysPrxForUser", "_sys_vsnprintf", {dst, 6, fmt, va}), 12);  // would-be length
    CHECK(GetString(m, dst) == "-42-a");
    CHECK_EQ(m.memory.Read<u8>(dst + 6), 0xCC);  // nothing past n
    CHECK_EQ(Hle(m, "sysPrxForUser", "_sys_vsnprintf", {dst, 0, fmt, va}), 12);
    const std::size_t before = m.process.tty_output.size();
    CHECK_EQ(Hle(m, "sysPrxForUser", "_sys_vprintf", {fmt, va}), 12);
    CHECK(m.process.tty_output.substr(before) == "-42-abc-beef");

    PutString(m, dst, "foo");
    PutString(m, str, "barbaz");
    CHECK_EQ(Hle(m, "sysPrxForUser", "_sys_strncat", {dst, str, 3}), dst);
    CHECK(GetString(m, dst) == "foobar");
    CHECK_EQ(Hle(m, "sysPrxForUser", "_sys_strncat", {dst, str, 100}), dst);
    CHECK(GetString(m, dst) == "foobarbarbaz");
}

void TestSpuPrintf() {
    std::printf("-- _sys_spu_printf_*: callbacks stored and called with the group/thread id\n");
    Machine m;
    hle::SysSpuHle spu_hle(*m.kernel);
    spu_hle.Register(*m.hle);
    using namespace as;
    // Four guest callbacks: attach_group = id + 1, detach_group = id + 2, attach_thread = id + 3,
    // detach_thread = id + 4. OPDs at kData + 0x40.., code at kCode + 0x400..
    for (u32 i = 0; i < 4; ++i) {
        const u32 code = kCode + 0x400 + 0x20 * i;
        m.Load({addi(3, 3, static_cast<s16>(i + 1)), blr()}, code);
        m.memory.Write<u32>(kData + 0x40 + 8 * i, code);
        m.memory.Write<u32>(kData + 0x44 + 8 * i, 0);
    }
    CHECK_EQ(static_cast<s32>(Hle(m, "sysPrxForUser", "_sys_spu_printf_attach_group", {0x100})), CELL_ESTAT);
    CHECK_EQ(static_cast<s32>(Hle(m, "sysPrxForUser", "_sys_spu_printf_finalize", {})), CELL_ESTAT);
    CHECK_EQ(static_cast<s32>(Hle(m, "sysPrxForUser", "_sys_spu_printf_initialize",
                                  {kData + 0x40, kData + 0x48, kData + 0x50, kData + 0x58})),
             CELL_OK);
    CHECK_EQ(Hle(m, "sysPrxForUser", "_sys_spu_printf_attach_group", {0x100}), 0x101);
    CHECK_EQ(Hle(m, "sysPrxForUser", "_sys_spu_printf_detach_group", {0x100}), 0x102);
    CHECK_EQ(Hle(m, "sysPrxForUser", "_sys_spu_printf_attach_thread", {0x200}), 0x203);
    CHECK_EQ(Hle(m, "sysPrxForUser", "_sys_spu_printf_detach_thread", {0x200}), 0x204);
    CHECK_EQ(Hle(m, "sysPrxForUser", "sys_spu_printf_attach_group", {0x300}), 0x301);  // legacy name
    CHECK_EQ(static_cast<s32>(Hle(m, "sysPrxForUser", "_sys_spu_printf_finalize", {})), CELL_OK);
    CHECK_EQ(static_cast<s32>(Hle(m, "sysPrxForUser", "_sys_spu_printf_attach_group", {0x100})), CELL_ESTAT);
}

void TestLibprof() {
    std::printf("-- cellLibprof cellUserTrace* accepted\n");
    Machine m;
    hle::CellLibprof libprof;
    libprof.Register(*m.hle);
    for (const char* n : {"cellUserTraceInit", "cellUserTraceRegister", "cellUserTraceUnregister", "cellUserTraceTerminate"}) {
        const u32 index = m.hle->Bind("cellLibprof", hle::ComputeFnid(n));
        CHECK(m.hle->At(index).implemented);
        CHECK_EQ(static_cast<s32>(Hle(m, "cellLibprof", n, {1, 2})), CELL_OK);
    }
}

bool Contains(const std::string& text, const char* what) {
    if (text.find(what) != std::string::npos) return true;
    std::fprintf(stderr, "report lacks \"%s\":\n%s\n", what, text.c_str());
    return false;
}

void TestStallReport() {
    std::printf("-- stall report: blocked PPU thread, its syscall, caller and queue\n");
    Machine m;
    using namespace as;
    // queue
    m.memory.Write<u32>(kData + 0x300, lv2::SYS_SYNC_FIFO);
    m.memory.Write<u32>(kData + 0x304, lv2::SYS_PPU_QUEUE);
    m.memory.Write<u64>(kData + 0x308, 0);
    CHECK_EQ(Sys(m, 128, {kData + 0x400, kData + 0x300, 0x1234, 16}), CELL_OK);
    const u32 queue = m.memory.Read<u32>(kData + 0x400);
    // thread: bl to a helper that does sys_event_queue_receive(queue, 0, 0), then returns
    const u32 entry = kCode + 0x800, helper = kCode + 0x840;
    m.Load({mflr(0), std_(0, 16, 1), stdu(1, -112, 1), bl(static_cast<s32>(helper - (entry + 12))), addi(1, 1, 112), ld(0, 16, 1),
            mtlr(0), blr()},
           entry);
    m.Load({lis(3, static_cast<s16>(queue >> 16)), ori(3, 3, static_cast<u16>(queue)), li(4, 0), li(5, 0), li(11, 130), sc(), blr()},
           helper);
    m.memory.Write<u32>(kData + 0x500, entry);
    m.memory.Write<u32>(kData + 0x504, 0);
    lv2::ThreadManager::CreateParams p;
    p.entry_opd = kData + 0x500;
    p.name = "blocked_reader";
    u32 id = 0;
    CHECK_EQ(m.kernel->threads().Create(p, &id), CELL_OK);
    CHECK_EQ(m.kernel->threads().Start(id), CELL_OK);
    bool blocked = false;
    for (int i = 0; i < 500 && !blocked; ++i) {
        for (const auto& t : m.kernel->threads().Snapshot())
            if (t.id == id && t.live_kind == ppu::PpuLiveStatus::kInSyscall && t.live_call == 130) blocked = true;
        if (!blocked) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(blocked);

    lv2::StallMonitor monitor(*m.kernel, 1);
    monitor.AddSection("extra", [] { return std::vector<std::string>{"section line"}; });
    const std::string report = monitor.BuildReport("test");
    CHECK(Contains(report, "BLOCKED in syscall 130 sys_event_queue_receive"));
    CHECK(Contains(report, "blocked_reader"));
    char lr[32];
    std::snprintf(lr, sizeof(lr), "lr 0x%08x", entry + 16);  // return address of the bl
    CHECK(Contains(report, lr));
    CHECK(Contains(report, "1 waiter(s), 0 pending event(s)"));
    CHECK(Contains(report, "section line"));

    // The monitor notices the stall once (progress does not move while the thread is blocked).
    monitor.Start();
    for (int i = 0; i < 300 && monitor.reports_printed() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(monitor.reports_printed(), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK_EQ(monitor.reports_printed(), 1);  // once per episode
    monitor.Stop();

    // Wake it: destroy the queue (force); the thread returns and exits.
    CHECK_EQ(Sys(m, 129, {queue, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE}), CELL_OK);
    m.kernel->threads().WaitForAllThreads();
}

}  // namespace

int main() {
    log::Threshold() = log::Level::Error;
    TestNids();
    TestLockLineSyscall();
    TestParamSfo();
    TestStringHle();
    TestSpuPrintf();
    TestLibprof();
    TestStallReport();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all libsre support tests passed\n");
    return 0;
}
