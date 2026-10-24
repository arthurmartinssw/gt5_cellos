// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/paging_tests.cpp
//
// Phase 7.3: demand paging through sys_mmapper page-fault notification — the boot pattern
// that crashed GT5 (reserve an area, register a pager queue, touch the area):
//   * syscall 327 argument checks (EINVAL / ESRCH / EBUSY);
//   * a PPU thread storing into the unmapped area is suspended, the queue receives
//     {0xFFFE000000000000, address, (0 << 32) | thread id, NON_MAPPED}; a recover without a
//     mapping re-faults; after map + sys_ppu_thread_recover_page_fault the same store
//     completes and the thread runs on;
//   * read-only mapping: cause READ_ONLY, the pager remaps read-write;
//   * an SPU thread's MFC GET from the area is suspended the same way (type 1, SPU thread id)
//     and resumed with sys_spu_thread_recover_page_fault;
//   * without a notification the access is still a fatal data fault;
//   * sys_mmapper_free_address refuses an area with a suspended thread.
#include <chrono>
#include <thread>

#include "hle/fnid.h"
#include "hle/sys_spu_hle.h"
#include "lv2/sys_event.h"
#include "lv2/sys_mmapper.h"
#include "lv2/sys_memory.h"
#include "lv2/sys_spu.h"
#include "ppu_asm.h"
#include "spu/spu_interpreter.h"
#include "spu_asm.h"
#include "test_machine.h"

using namespace cell;

namespace {

constexpr u64 kRw = lv2::SYS_MEMORY_PROT_READ_WRITE | lv2::SYS_MEMORY_PAGE_SIZE_1M;
constexpr u64 kRo = lv2::SYS_MEMORY_PROT_READ_ONLY | lv2::SYS_MEMORY_PAGE_SIZE_1M;

s32 Sys(Machine& m, u32 n, std::initializer_list<u64> args, ppu::PpuThreadContext* out = nullptr) {
    ppu::PpuThreadContext c{};
    c.gpr[11] = n;
    int r = 3;
    for (u64 a : args) c.gpr[r++] = a;
    m.syscalls.Dispatch(c);
    if (out) *out = c;
    return static_cast<s32>(c.gpr[3]);
}

u32 MakeQueue(Machine& m) {
    m.memory.Write<u32>(kData + 0x300, lv2::SYS_SYNC_FIFO);
    m.memory.Write<u32>(kData + 0x304, lv2::SYS_PPU_QUEUE);
    m.memory.Write<u64>(kData + 0x308, 0);
    CHECK_EQ(Sys(m, 128, {kData + 0x310, kData + 0x300, 0, 16}), CELL_OK);
    return m.memory.Read<u32>(kData + 0x310);
}

u32 AllocateArea(Machine& m) {
    CHECK_EQ(Sys(m, 330, {0x10000000, lv2::SYS_MEMORY_PAGE_SIZE_1M, 0x10000000, kData + 0x320}), CELL_OK);
    return m.memory.Read<u32>(kData + 0x320);
}

u32 MapNew(Machine& m, u32 at, u64 flags) {
    CHECK_EQ(Sys(m, 332, {lv2::SYS_MMAPPER_NO_SHM_KEY, 0x100000, flags, kData + 0x324}), CELL_OK);
    const u32 id = m.memory.Read<u32>(kData + 0x324);
    CHECK_EQ(Sys(m, 334, {at, id, flags}), CELL_OK);
    return id;
}

// Blocking sys_event_queue_receive on the test thread: {source, data1, data2, data3}.
std::array<u64, 4> Receive(Machine& m, u32 queue) {
    ppu::PpuThreadContext c{};
    CHECK_EQ(Sys(m, 130, {queue, 0, 0}, &c), CELL_OK);
    return {c.gpr[4], c.gpr[5], c.gpr[6], c.gpr[7]};
}

// PPU thread: r28 = 0x1234; stw r28,0x40(area); r3 = lwz 0x40(area); return r3.
u32 StartStorer(Machine& m, u32 area) {
    using namespace as;
    const u32 code = kCode + 0x800;
    m.Load({lis(10, static_cast<s32>(area >> 16)), ori(10, 10, area & 0xFFFF), li(28, 0x1234), stw(28, 0x40, 10), lwz(3, 0x40, 10),
            blr()},
           code);
    m.memory.Write<u32>(kData + 0x500, code);
    m.memory.Write<u32>(kData + 0x504, 0);
    lv2::ThreadManager::CreateParams p;
    p.entry_opd = kData + 0x500;
    p.flags = lv2::SYS_PPU_THREAD_CREATE_JOINABLE;
    p.name = "storer";
    u32 id = 0;
    CHECK_EQ(m.kernel->threads().Create(p, &id), CELL_OK);
    CHECK_EQ(m.kernel->threads().Start(id), CELL_OK);
    return id;
}

u64 Join(Machine& m, u32 id) {
    ppu::PpuThreadContext host{};
    u64 value = 0;
    CHECK_EQ(m.kernel->threads().Join(host, id, &value), CELL_OK);
    return value;
}

void TestPpuDemandPaging() {
    std::printf("-- PPU store into an unmapped notified area: suspend, event, map, recover, retry\n");
    Machine m;
    const u32 q = MakeQueue(m), area = AllocateArea(m);
    CHECK_EQ(Sys(m, 327, {area + 0x100000, q}), CELL_EINVAL);  // not the base of an area
    CHECK_EQ(Sys(m, 327, {area, 0x8D0000FF}), CELL_ESRCH);
    CHECK_EQ(Sys(m, 327, {area, q}), CELL_OK);
    CHECK_EQ(Sys(m, 327, {area, q}), CELL_EBUSY);

    const u32 tid = StartStorer(m, area);
    auto e = Receive(m, q);
    CHECK_EQ(e[0], lv2::SYS_MEMORY_PAGE_FAULT_EVENT_KEY);
    CHECK_EQ(e[1], area + 0x40);
    CHECK_EQ(e[2], tid);  // type 0 (PPU) in the upper word
    CHECK_EQ(e[3], lv2::SYS_MEMORY_PAGE_FAULT_CAUSE_NON_MAPPED);
    CHECK(m.kernel->mmapper().IsPageFaulted(lv2::SYS_MEMORY_PAGE_FAULT_TYPE_PPU_THREAD, tid));
    bool shown = false;
    for (const auto& t : m.kernel->threads().Snapshot())
        if (t.id == tid) shown = t.live_kind == ppu::PpuLiveStatus::kInPageFault && t.live_args[0] == area + 0x40 && t.live_args[1] == 1;
    CHECK(shown);
    CHECK_EQ(Sys(m, 331, {area}), CELL_EBUSY);  // free_address with a suspended thread

    // recover without mapping: the store faults again
    CHECK_EQ(Sys(m, 57, {tid}), CELL_OK);
    e = Receive(m, q);
    CHECK_EQ(e[1], area + 0x40);

    MapNew(m, area, kRw);
    CHECK_EQ(Sys(m, 57, {tid}), CELL_OK);
    CHECK_EQ(Join(m, tid), 0x1234);
    CHECK_EQ(m.memory.Read<u32>(area + 0x40), 0x1234);
    CHECK_EQ(Sys(m, 57, {tid}), CELL_ESRCH);         // joined: gone
    CHECK_EQ(Sys(m, 57, {0x01000000}), CELL_ESRCH);  // no such thread
    CHECK(!m.kernel->is_terminating());
}

void TestReadOnlyCause() {
    std::printf("-- PPU store into a read-only mapping: cause READ_ONLY, pager remaps read-write\n");
    Machine m;
    const u32 q = MakeQueue(m), area = AllocateArea(m);
    CHECK_EQ(Sys(m, 327, {area, q}), CELL_OK);
    const u32 ro = MapNew(m, area, kRo);
    const u32 tid = StartStorer(m, area);
    const auto e = Receive(m, q);
    CHECK_EQ(e[1], area + 0x40);
    CHECK_EQ(e[3], lv2::SYS_MEMORY_PAGE_FAULT_CAUSE_READ_ONLY);
    CHECK_EQ(Sys(m, 335, {area, kData + 0x328}), CELL_OK);
    CHECK_EQ(Sys(m, 334, {area, ro, kRw}), CELL_OK);
    CHECK_EQ(Sys(m, 57, {tid}), CELL_OK);
    CHECK_EQ(Join(m, tid), 0x1234);
}

void TestNoNotificationIsFatal() {
    std::printf("-- without a notification the access is still a fatal data fault\n");
    Machine m;
    const u32 area = AllocateArea(m);
    const u32 tid = StartStorer(m, area);
    for (int i = 0; i < 500 && !m.kernel->is_terminating(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(m.kernel->is_terminating());
    const auto end = m.kernel->process_end();
    CHECK(end.fatal);
    CHECK(end.thread_id == tid);
    CHECK(end.stop.reason == ppu::StopReason::DataFault);
    CHECK_EQ(end.stop.address, area + 0x40);
}

void TestSpuDemandPaging() {
    std::printf("-- SPU MFC GET from the notified area: suspend, event (type 1), map, recover, resume\n");
    Machine m;
    hle::SysSpuHle spu_hle(*m.kernel);
    spu_hle.Register(*m.hle);
    m.kernel->spu().SetExecutorFactory(spu::MakeSpuInterpreter);
    const u32 q = MakeQueue(m), area = AllocateArea(m);
    CHECK_EQ(Sys(m, 327, {area, q}), CELL_OK);

    // SPU: GET 16 bytes from area+0x80 into LS 0x2000, wait for the tag, exit with word 0.
    using namespace sa;
    std::vector<u32> code{Il(10, 0x2000), Wrch(16, 10), Il(11, 0), Wrch(17, 11)};
    for (u32 w : Load32(12, area + 0x80)) code.push_back(w);
    code.insert(code.end(), {Wrch(18, 12), Il(13, 16), Wrch(19, 13), Il(14, 0), Wrch(20, 14), Il(15, 0x40), Wrch(21, 15)});
    code.insert(code.end(), {Il(17, 1), Wrch(22, 17), Il(18, 2), Wrch(23, 18), Rdch(19, 24), Lqd(16, 0, 10)});
    code.insert(code.end(), {Wrch(28, 16), Stop(0x102)});

    const u32 elf = kData + 0x1000, payload = 0x80;
    m.memory.Fill(elf, 0, 0x100);
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
    const u32 index = m.hle->Bind("sysPrxForUser", hle::ComputeFnid("sys_spu_image_import"));
    const auto& f = m.hle->At(index);
    ppu::PpuThreadContext c{};
    c.gpr[3] = img;
    c.gpr[4] = elf;
    c.gpr[5] = spu::SYS_SPU_IMAGE_PROTECT;
    f.fn(c, m.memory, f);
    CHECK_EQ(static_cast<s32>(c.gpr[3]), CELL_OK);
    m.memory.Fill(kData + 0x500, 0, 0x60);
    CHECK_EQ(Sys(m, 170, {kData + 0x580, 1, 100, kData + 0x500}), CELL_OK);
    const u32 group = m.memory.Read<u32>(kData + 0x580);
    CHECK_EQ(Sys(m, 172, {kData + 0x584, group, 0, img, kData + 0x520, kData + 0x530}), CELL_OK);
    const u32 thread = m.memory.Read<u32>(kData + 0x584);
    CHECK_EQ(Sys(m, 173, {group}), CELL_OK);

    const auto e = Receive(m, q);
    CHECK_EQ(e[0], lv2::SYS_MEMORY_PAGE_FAULT_EVENT_KEY);
    CHECK_EQ(e[1], area + 0x80);
    CHECK_EQ(e[2], (u64{1} << 32) | thread);
    CHECK_EQ(e[3], lv2::SYS_MEMORY_PAGE_FAULT_CAUSE_NON_MAPPED);
    MapNew(m, area, kRw);
    m.memory.Write<u32>(area + 0x80, 0xCAFEF00D);
    CHECK_EQ(Sys(m, 198, {thread}), CELL_OK);
    CHECK_EQ(Sys(m, 178, {group, kData + 0x590, kData + 0x594}), CELL_OK);  // join the group
    CHECK_EQ(Sys(m, 165, {thread, kData + 0x598}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kData + 0x598), 0xCAFEF00D);
    CHECK(!m.kernel->is_terminating());
}

void TestTerminationWakes() {
    std::printf("-- a thread suspended on a page fault is released when the process ends\n");
    Machine m;
    const u32 q = MakeQueue(m), area = AllocateArea(m);
    CHECK_EQ(Sys(m, 327, {area, q}), CELL_OK);
    const u32 tid = StartStorer(m, area);
    Receive(m, q);
    CHECK(m.kernel->mmapper().IsPageFaulted(lv2::SYS_MEMORY_PAGE_FAULT_TYPE_PPU_THREAD, tid));
    lv2::ProcessEnd end;
    m.kernel->RequestTermination(end);
    m.kernel->Shutdown();  // must not hang
    CHECK(!m.kernel->mmapper().IsPageFaulted(lv2::SYS_MEMORY_PAGE_FAULT_TYPE_PPU_THREAD, tid));
}

}  // namespace

int main() {
    log::Threshold() = log::Level::Error;
    TestPpuDemandPaging();
    TestReadOnlyCause();
    TestNoNotificationIsFatal();
    TestSpuDemandPaging();
    TestTerminationWakes();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all demand paging tests passed\n");
    return 0;
}
