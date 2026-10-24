// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/spu_lifecycle_tests.cpp
//
// Phase 7.5: the SPU thread-group lifecycle behind the repeated SPURS start/exit lines, and
// the diagnostics added for it.
//   * state machine: start while running -> ESTAT; start after the group exited but before
//     join -> ESTAT (a faulty handler cannot restart a group that has not been joined);
//     join -> may start again; host threads are reaped by join;
//   * legitimate repeated scheduling: 50 start/join cycles of a group whose SPU program
//     increments a word of its own image and exits with it — every run reports the same
//     value, so the image is re-deployed on each start, as lv2 does;
//   * run statistics: starts, completed runs, exact SPU instructions per run, exit causes
//     (GROUP_EXIT and TERMINATED), and the bounded-logging decision per run;
//   * SpuRunStats summary text (rate, averages, interval range, heuristic hint);
//   * DescribeCallSite (the caller instruction now printed with unimplemented imports).
#include <chrono>
#include <thread>

#include "hle/fnid.h"
#include "hle/hle_registry.h"
#include "hle/sys_spu_hle.h"
#include "lv2/sys_spu.h"
#include "ppu_asm.h"
#include "spu/spu_interpreter.h"
#include "spu/spu_run_stats.h"
#include "spu_asm.h"
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

struct Group {
    Machine m;
    hle::SysSpuHle spu_hle{*m.kernel};
    u32 group = 0, thread = 0;

    // One SPU thread running `code` (loaded at LS 0x100 as a single segment).
    explicit Group(const std::vector<u32>& code) {
        spu_hle.Register(*m.hle);
        m.kernel->spu().SetExecutorFactory(spu::MakeSpuInterpreter);
        const u32 elf = kData + 0x1000, payload = 0x80;
        m.memory.Fill(elf, 0, 0x200);
        const u8 ident[] = {0x7F, 'E', 'L', 'F', 1, 2, 1};
        m.memory.CopyIn(elf, ident, sizeof(ident));
        m.memory.Write<u16>(elf + 16, 2);
        m.memory.Write<u16>(elf + 18, 23);
        m.memory.Write<u32>(elf + 24, 0x100);
        m.memory.Write<u32>(elf + 28, 52);
        m.memory.Write<u16>(elf + 42, 32);
        m.memory.Write<u16>(elf + 44, 1);
        m.memory.Write<u32>(elf + 52, 1);
        m.memory.Write<u32>(elf + 56, payload);
        m.memory.Write<u32>(elf + 60, 0x100);
        m.memory.Write<u32>(elf + 68, static_cast<u32>(code.size() * 4));
        m.memory.Write<u32>(elf + 72, static_cast<u32>(code.size() * 4));
        for (std::size_t k = 0; k < code.size(); ++k) m.memory.Write<u32>(elf + payload + 4 * static_cast<u32>(k), code[k]);
        const u32 img = kData + 0x400;
        const auto& f = m.hle->At(m.hle->Bind("sysPrxForUser", hle::ComputeFnid("sys_spu_image_import")));
        ppu::PpuThreadContext c{};
        c.gpr[3] = img;
        c.gpr[4] = elf;
        c.gpr[5] = spu::SYS_SPU_IMAGE_PROTECT;
        f.fn(c, m.memory, f);
        CHECK_EQ(static_cast<s32>(c.gpr[3]), CELL_OK);
        m.memory.Fill(kData + 0x500, 0, 0x60);
        m.memory.CopyIn(kData + 0x5C0, "test-group", 11);
        m.memory.Write<u32>(kData + 0x500, 10);         // name length
        m.memory.Write<u32>(kData + 0x504, kData + 0x5C0);  // name
        CHECK_EQ(Sys(m, 170, {kData + 0x580, 1, 100, kData + 0x500}), CELL_OK);
        group = m.memory.Read<u32>(kData + 0x580);
        CHECK_EQ(Sys(m, 172, {kData + 0x584, group, 0, img, kData + 0x520, kData + 0x530}), CELL_OK);
        thread = m.memory.Read<u32>(kData + 0x584);
    }
    std::shared_ptr<spu::SpuThreadGroup> G() { return m.kernel->spu().FindGroup(group); }
    bool WaitStopped() {
        for (int i = 0; i < 2000; ++i) {
            {
                auto g = G();
                std::lock_guard lock(g->m);
                if (g->state == spu::SpuThreadGroup::State::Stopped) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
    std::pair<s32, s32> Join() {
        CHECK_EQ(Sys(m, 178, {group, kData + 0x590, kData + 0x594}), CELL_OK);
        return {static_cast<s32>(m.memory.Read<u32>(kData + 0x590)), static_cast<s32>(m.memory.Read<u32>(kData + 0x594))};
    }
};

// LS 0x140 holds 41 in the image; the program loads it, adds 1, stores it back and exits the
// group with it as the status: 42 on every run iff the image is re-deployed at each start.
std::vector<u32> IncrementProgram() {
    using namespace sa;
    std::vector<u32> code{Il(10, 0x140), Lqd(5, 0, 10), Ai(5, 5, 1), Stqd(5, 0, 10), Wrch(28, 5), Stop(0x101)};
    code.resize(0x40 / 4, 0);
    code.insert(code.end(), {41, 0, 0, 0});
    return code;
}

void TestRestartSemantics() {
    std::printf("-- group lifecycle: no restart before join, 50 legitimate start/join cycles, image re-deployed\n");
    auto& cfg = spu::RunLogConfig();
    cfg.first_runs.store(3);
    cfg.verbose.store(false);
    Group g(IncrementProgram());
    CHECK_EQ(Sys(g.m, 173, {g.group}), CELL_OK);
    CHECK(g.WaitStopped());
    CHECK_EQ(Sys(g.m, 173, {g.group}), CELL_ESTAT);  // exited, not joined: cannot be restarted
    auto [cause, status] = g.Join();
    CHECK_EQ(cause, spu::SYS_SPU_THREAD_GROUP_JOIN_GROUP_EXIT);
    CHECK_EQ(status, 42);
    {
        auto grp = g.G();
        std::lock_guard lock(grp->m);
        CHECK(grp->hosts.empty());  // join reaped the host thread
        CHECK(grp->state == spu::SpuThreadGroup::State::Initialized);
    }
    CHECK_EQ(Sys(g.m, 178, {g.group, kData + 0x590, kData + 0x594}), CELL_ESTAT);  // nothing to join

    bool all_42 = true;
    for (int i = 0; i < 50; ++i) {
        CHECK_EQ(Sys(g.m, 173, {g.group}), CELL_OK);
        const auto r = g.Join();
        all_42 &= r.first == spu::SYS_SPU_THREAD_GROUP_JOIN_GROUP_EXIT && r.second == 42;
        if (i == 0) CHECK(g.G()->log_this_run.load());   // runs 1..3 are logged
        if (i == 5) CHECK(!g.G()->log_this_run.load());  // later runs are counted
    }
    CHECK(all_42);
    auto grp = g.G();
    std::lock_guard lock(grp->m);
    CHECK_EQ(grp->stats.starts, 51);
    CHECK_EQ(grp->stats.runs, 51);
    CHECK_EQ(grp->stats.last_insns, 6);  // il, lqd, ai, stqd, wrch, stop
    CHECK_EQ(grp->stats.total_insns, 51 * 6);
    CHECK_EQ((grp->stats.exits[{spu::SYS_SPU_THREAD_GROUP_JOIN_GROUP_EXIT, 42}]), 51);
    CHECK(grp->hosts.empty());
}

void TestStartWhileRunningAndTerminate() {
    std::printf("-- start while running -> ESTAT; terminate counted as a TERMINATED run\n");
    using namespace sa;
    Group g({Br(0)});  // spins until terminated
    CHECK_EQ(Sys(g.m, 173, {g.group}), CELL_OK);
    CHECK_EQ(Sys(g.m, 173, {g.group}), CELL_ESTAT);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_EQ(Sys(g.m, 177, {g.group, 7}), CELL_OK);
    const auto [cause, status] = g.Join();
    CHECK_EQ(cause, spu::SYS_SPU_THREAD_GROUP_JOIN_TERMINATED);
    CHECK_EQ(status, 7);
    {
        auto grp = g.G();
        std::lock_guard lock(grp->m);
        CHECK_EQ(grp->stats.runs, 1);
        CHECK((grp->stats.exits[{spu::SYS_SPU_THREAD_GROUP_JOIN_TERMINATED, 7}]) == 1);
        CHECK(grp->stats.last_insns > 0);
    }
    bool described = false;  // Describe() takes the group lock itself
    for (const std::string& l : g.m.kernel->spu().Describe())
        described |= l.find("1 start(s), 1 completed run(s)") != std::string::npos && l.find("TERMINATED(7) x1") != std::string::npos;
    CHECK(described);
}

void TestSummaryText() {
    std::printf("-- SpuRunStats summary: rate, averages, interval range, exits, heuristic hint\n");
    {
        spu::SpuRunStats s;
        // 3 runs, 5.333 ms apart, 0.2 ms each, 20000 instructions each: a paced dispatch cycle
        for (int i = 0; i < 3; ++i) {
            const u64 t = 1'000'000 + static_cast<u64>(i) * 5333;
            s.OnStart(t);
            s.OnStop(t + 200, 20000, spu::SYS_SPU_THREAD_GROUP_JOIN_GROUP_EXIT, 0);
        }
        CHECK(!s.WindowDue(1'000'000 + 11'000, 10));
        CHECK(s.WindowDue(11'000'000, 10));
        CHECK(!s.WindowDue(11'000'000, 0));
        const std::string line = s.TakeWindowSummary(1'016'000, 0x04000002, "sgx-spurs-CellSpursKernelGroup");
        CHECK(line.find("0x04000002 \"sgx-spurs-CellSpursKernelGroup\": 3 run(s)") != std::string::npos);
        CHECK(line.find("avg 20000 SPU insns/run") != std::string::npos);
        CHECK(line.find("avg run 0.200 ms") != std::string::npos);
        CHECK(line.find("start interval 5.33..5.33 ms") != std::string::npos);
        CHECK(line.find("GROUP_EXIT(0) x3") != std::string::npos);
        CHECK(line.find("heuristic") == std::string::npos);
        CHECK_EQ(s.window_runs, 0);  // new window
        CHECK_EQ(s.runs, 3);
    }
    {
        spu::SpuRunStats s;
        // 5000 runs in 1 s, 100 instructions each: flagged
        for (int i = 0; i < 5000; ++i) {
            const u64 t = 1'000'000 + static_cast<u64>(i) * 200;
            s.OnStart(t);
            s.OnStop(t + 10, 100, spu::SYS_SPU_THREAD_GROUP_JOIN_GROUP_EXIT, 0);
        }
        const std::string line = s.TakeWindowSummary(2'000'000, 1, "g");
        CHECK(line.find("(5000.0/s)") != std::string::npos);
        CHECK(line.find("heuristic: very short runs at a high rate") != std::string::npos);
        CHECK(s.Totals().find("5000 start(s), 5000 completed run(s), avg 100 SPU insns/run") != std::string::npos);
    }
}

void TestCallSite() {
    std::printf("-- DescribeCallSite: the caller of an import, disassembled\n");
    Machine m;
    m.Load({as::nop(), as::bl(0x40), as::nop()}, kCode + 0x100);
    const std::string s = hle::DescribeCallSite(m.memory, kCode + 0x108);
    CHECK(s.find("0x00010104: ") == 0);
    CHECK(s.find("bl") != std::string::npos);
    CHECK(hle::DescribeCallSite(m.memory, 0xB0000004).empty());  // unmapped
    CHECK(hle::DescribeCallSite(m.memory, 2).empty());
}

}  // namespace

int main() {
    log::Threshold() = log::Level::Error;
    TestRestartSemantics();
    TestStartWhileRunningAndTerminate();
    TestSummaryText();
    TestCallSite();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all SPU lifecycle tests passed\n");
    return 0;
}
