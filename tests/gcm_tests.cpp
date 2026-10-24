// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/gcm_tests.cpp
//
// Phase 5: sys_mmapper (lv2 + sysPrxForUser wrappers), cellGcmSys HLE and the RSX FIFO front
// end. The FIFO tests write real command words into the command buffer, publish them through
// CellGcmControl.put exactly like libgcm's inlines, and check what the RSX thread does with
// them (reference, semaphores, call/return, flips, segment wrap, faults).
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <thread>

#include "common/host_thread.h"
#include "hle/cell_gcm_sys.h"
#include "hle/fnid.h"
#include "hle/gcm_interrupts.h"
#include "lv2/sys_ppu_thread.h"
#include "lv2/sys_mmapper.h"
#include "rsx/rsx_methods.h"
#include "ppu_asm.h"
#include "test_machine.h"

using namespace cell;

namespace {

class Watchdog {
public:
    explicit Watchdog(const char* what, int seconds = 20) : what_(what) {
        thread_ = std::thread([this, seconds] {
            for (int i = 0; i < seconds * 10 && !done_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!done_.load()) {
                std::fprintf(stderr, "FAIL watchdog: \"%s\" hung\n", what_);
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

s32 Sys(Machine& m, u32 number, std::initializer_list<u64> args) {
    ppu::PpuThreadContext c{};
    c.gpr[11] = number;
    int i = 3;
    for (u64 a : args) c.gpr[i++] = a;
    m.syscalls.Dispatch(c);
    return static_cast<s32>(c.gpr[3]);
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
s32 HleR(Machine& m, const char* module, const char* name, std::initializer_list<u64> args) {
    return static_cast<s32>(Hle(m, module, name, args).gpr[3]);
}

// Guest words shared with the RSX thread.
u32 Load(Machine& m, u32 ea) {
    return ByteSwap(std::atomic_ref<u32>(*reinterpret_cast<u32*>(m.memory.shadow() + ea)).load(std::memory_order_acquire));
}
void Store(Machine& m, u32 ea, u32 v) {
    std::atomic_ref<u32>(*reinterpret_cast<u32*>(m.memory.shadow() + ea)).store(ByteSwap(v), std::memory_order_release);
}

// Deadline-based: the number of loop iterations says nothing about elapsed time (on Windows
// a sub-millisecond sleep_for can return at once, or last a whole 15.6 ms tick).
template <typename Pred>
bool WaitFor(Pred pred, int ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        host::SleepMicroseconds(100);
    }
    return pred();
}

constexpr u32 kOut = kData + 0x100;  // u32 out slots
constexpr u32 kCtxPtr = kData + 0x200; // the game's gCellGcmCurrentContext

// Reserves a 256 MiB address area and maps `size` bytes at its start; returns the address.
u32 MapIoArea(Machine& m, u32 size, u32* area_out = nullptr) {
    CHECK_EQ(Sys(m, 330, {0x10000000, lv2::SYS_MEMORY_PAGE_SIZE_1M, 0x10000000, kOut}), CELL_OK);
    const u32 area = m.memory.Read<u32>(kOut);
    CHECK_EQ(HleR(m, "sysPrxForUser", "sys_mmapper_allocate_memory", {size, lv2::SYS_MEMORY_PAGE_SIZE_1M, kOut + 4}), CELL_OK);
    const u32 id = m.memory.Read<u32>(kOut + 4);
    CHECK_EQ(HleR(m, "sysPrxForUser", "sys_mmapper_map_memory", {area, id, lv2::SYS_MEMORY_PROT_READ_WRITE}), CELL_OK);
    if (area_out) *area_out = area;
    return area;
}

// ---------------------------------------------------------------------------------------

void TestMmapper() {
    std::printf("-- sys_mmapper: address areas, memory objects, map/unmap/search, budget\n");
    Machine m;
    CHECK_EQ(Sys(m, 330, {0x1000000, 0x400, 0, kOut}), CELL_EALIGN);
    CHECK_EQ(Sys(m, 330, {0x10000000, 0x800, 0, kOut}), CELL_EINVAL);
    CHECK_EQ(Sys(m, 330, {0x10000000, 0x400, 0x8000000, kOut}), CELL_EALIGN);
    CHECK_EQ(Sys(m, 330, {0x10000000, 0x400, 0, kOut}), CELL_OK);
    const u32 area = m.memory.Read<u32>(kOut);
    CHECK(area >= 0x40000000 && area < 0xC0000000 && area % 0x10000000 == 0);
    CHECK(!m.memory.Check(area, 4, vm::kPageReadable));  // reserved, not mapped

    const u64 used_before = m.memory.user_used();
    CHECK_EQ(HleR(m, "sysPrxForUser", "sys_mmapper_allocate_memory", {0x1234000, 0x400, kOut + 4}), CELL_EALIGN);
    CHECK_EQ(HleR(m, "sysPrxForUser", "sys_mmapper_allocate_memory", {0x1200000, 0x400, kOut + 4}), CELL_OK);
    const u32 id = m.memory.Read<u32>(kOut + 4);
    CHECK_EQ(m.memory.user_used() - used_before, 0x1200000);
    CHECK_EQ(HleR(m, "sysPrxForUser", "sys_mmapper_allocate_memory", {0x10000, 0x200, kOut + 8}), CELL_OK);
    const u32 small = m.memory.Read<u32>(kOut + 8);

    CHECK_EQ(HleR(m, "sysPrxForUser", "sys_mmapper_map_memory", {0x30000000, id, 0}), CELL_EINVAL);  // not an area
    CHECK_EQ(HleR(m, "sysPrxForUser", "sys_mmapper_map_memory", {area + 0x10000, id, 0}), CELL_EALIGN);
    CHECK_EQ(HleR(m, "sysPrxForUser", "sys_mmapper_map_memory", {area + 0x0FF00000, id, 0}), CELL_EINVAL);  // past the end
    CHECK_EQ(HleR(m, "sysPrxForUser", "sys_mmapper_map_memory", {area, id, lv2::SYS_MEMORY_PROT_READ_WRITE}), CELL_OK);
    CHECK(m.memory.Check(area, 0x1200000, vm::kPageWritable));
    CHECK_EQ(HleR(m, "sysPrxForUser", "sys_mmapper_map_memory", {area + 0x2000000, id, 0}), CELL_EBUSY);  // already mapped
    CHECK_EQ(HleR(m, "sysPrxForUser", "sys_mmapper_map_memory", {area + 0x100000, small, 0}), CELL_EBUSY);  // overlap
    m.memory.Write<u32>(area + 0x123450, 0xFEEDF00D);

    CHECK_EQ(Sys(m, 331, {area}), CELL_EBUSY);
    CHECK_EQ(HleR(m, "sysPrxForUser", "sys_mmapper_free_memory", {id}), CELL_EBUSY);
    CHECK_EQ(HleR(m, "sysPrxForUser", "sys_mmapper_unmap_memory", {area, kOut + 12}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut + 12), id);
    CHECK(!m.memory.Check(area, 4, vm::kPageReadable));
    // Small object first, then search_and_map finds the next free 1 MiB slot; contents survive.
    CHECK_EQ(Sys(m, 334, {area, small, 0}), CELL_OK);
    CHECK_EQ(Sys(m, 337, {area, id, 0, kOut + 16}), CELL_OK);
    const u32 again = m.memory.Read<u32>(kOut + 16);
    CHECK_EQ(again, area + 0x100000);
    CHECK_EQ(m.memory.Read<u32>(again + 0x123450), 0xFEEDF00D);
    CHECK_EQ(Sys(m, 335, {again, kOut + 12}), CELL_OK);
    CHECK_EQ(Sys(m, 335, {area, kOut + 12}), CELL_OK);
    CHECK_EQ(Sys(m, 335, {area, kOut + 12}), CELL_EINVAL);
    CHECK_EQ(Sys(m, 329, {id}), CELL_OK);
    CHECK_EQ(Sys(m, 329, {small}), CELL_OK);
    CHECK_EQ(Sys(m, 329, {id}), CELL_ESRCH);
    CHECK_EQ(m.memory.user_used(), used_before);
    CHECK_EQ(Sys(m, 331, {area}), CELL_OK);
    CHECK_EQ(Sys(m, 331, {area}), CELL_EINVAL);
    CHECK_EQ(Sys(m, 326, {}), CELL_OK);
    CHECK_EQ(Sys(m, 326, {}), CELL_EEXIST);
}

void TestGcmInitAndIo() {
    std::printf("-- cellGcm: _cellGcmInitBody context, configuration, IO mapping, offset tables\n");
    Machine m;
    hle::CellGcmSys gcm(*m.kernel);
    gcm.Register(*m.hle);
    u32 area = 0;
    const u32 io = MapIoArea(m, 0x200000, &area);  // 2 MiB io area (1 MiB used by init)

    // Failure modes first: nothing must be half-initialized afterwards.
    CHECK_EQ(HleR(m, "cellGcmSys", "_cellGcmInitBody", {kCtxPtr, 0x10000, 0x100000, io + 0x1000}), hle::CELL_GCM_ERROR_FAILURE);
    CHECK_EQ(HleR(m, "cellGcmSys", "_cellGcmInitBody", {kCtxPtr, 0x10000, 0x100000, 0x50000000}), hle::CELL_GCM_ERROR_FAILURE);
    CHECK_EQ(HleR(m, "cellGcmSys", "_cellGcmInitBody", {kCtxPtr, 0x200000, 0x100000, io}), hle::CELL_GCM_ERROR_FAILURE);
    CHECK(!gcm.initialized());

    CHECK_EQ(HleR(m, "cellGcmSys", "_cellGcmInitBody", {kCtxPtr, 0x10000, 0x100000, io}), CELL_OK);
    CHECK_EQ(HleR(m, "cellGcmSys", "_cellGcmInitBody", {kCtxPtr, 0x10000, 0x100000, io}), hle::CELL_GCM_ERROR_FAILURE);
    // The regression: the game's context pointer is set, and context->current is readable.
    const u32 ctx = m.memory.Read<u32>(kCtxPtr);
    CHECK(ctx != 0 && ctx == gcm.context_ea());
    CHECK_EQ(m.memory.Read<u32>(ctx + 0), io + 0x1000);      // begin
    CHECK_EQ(m.memory.Read<u32>(ctx + 4), io + 0x8000 - 4);  // end of the first 32 KiB segment
    CHECK_EQ(m.memory.Read<u32>(ctx + 8), io + 0x1000);      // current
    const u32 opd = m.memory.Read<u32>(ctx + 12);
    u32 index = 0;
    CHECK(hle::DecodeHleOpcode(m.memory.Read<u32>(m.memory.Read<u32>(opd)), &index));
    CHECK(m.hle->At(index).name == "cellGcmCallback");
    const u32 control = gcm.control_ea();
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmGetControlRegister", {}), control);
    CHECK_EQ(Load(m, control + 0), 0x1000);
    CHECK_EQ(Load(m, control + 4), 0x1000);
    CHECK_EQ(Load(m, control + 8), 0xFFFFFFFF);
    CHECK_EQ(static_cast<u32>(HleR(m, "cellGcmSys", "cellGcmGetLabelAddress", {3})), gcm.label_ea() + 0x30);

    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmGetConfiguration", {kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut + 0), rsx::kLocalMemoryBase);
    CHECK_EQ(m.memory.Read<u32>(kOut + 4), io);
    CHECK_EQ(m.memory.Read<u32>(kOut + 8), rsx::kLocalMemoryGameSize);
    CHECK_EQ(m.memory.Read<u32>(kOut + 12), 0x100000);
    CHECK(m.memory.Check(rsx::kLocalMemoryBase, rsx::kLocalMemoryGameSize, vm::kPageWritable));

    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmAddressToOffset", {io + 0x1234, kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut), 0x1234);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmAddressToOffset", {0xC0123450, kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut), 0x123450);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmAddressToOffset", {kData, kOut}), hle::CELL_GCM_ERROR_FAILURE);

    // Second megabyte of the io area mapped separately: next free IO page.
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmMapMainMemory", {io + 0x100000, 0x100000, kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut), 0x100000);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmMapMainMemory", {io + 0x100000, 0x100000, kOut}), hle::CELL_GCM_ERROR_FAILURE);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmMapMainMemory", {io + 0x80000, 0x100000, kOut}), hle::CELL_GCM_ERROR_FAILURE);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmMapMainMemory", {area + 0x8000000, 0x100000, kOut}), hle::CELL_GCM_ERROR_FAILURE);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmIoOffsetToAddress", {0x100010, kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut), io + 0x100010);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmGetOffsetTable", {kOut}), CELL_OK);
    const u32 io_table = m.memory.Read<u32>(kOut), ea_table = m.memory.Read<u32>(kOut + 4);
    CHECK_EQ(m.memory.Read<u16>(io_table + ((io >> 20) + 1) * 2), 1);
    CHECK_EQ(m.memory.Read<u16>(ea_table + 1 * 2), (io >> 20) + 1);
    CHECK_EQ(m.memory.Read<u16>(ea_table + 5 * 2), 0xFFFF);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmUnmapIoAddress", {0x100000}), CELL_OK);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmIoOffsetToAddress", {0x100010, kOut}), hle::CELL_GCM_ERROR_FAILURE);
    CHECK_EQ(m.memory.Read<u16>(io_table + ((io >> 20) + 1) * 2), 0xFFFF);

    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmGetTiledPitchSize", {0x500}), 0x500);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmGetTiledPitchSize", {0x501}), 0x600);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmGetTiledPitchSize", {0x10001}), 0);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetDisplayBuffer", {1, 0x200000, 5120, 1280, 720}), CELL_OK);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetDisplayBuffer", {8, 0, 0, 0, 0}), hle::CELL_GCM_ERROR_FAILURE);
    CHECK_EQ(m.memory.Read<u32>(gcm.display_info_ea() + 16 + 8), 1280);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetTileInfo", {15, 0, 0, 0x10000, 0x100, 0, 0, 0}), hle::CELL_GCM_ERROR_INVALID_VALUE);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetTileInfo", {0, 0, 0x8000, 0x10000, 0x100, 0, 0, 0}), hle::CELL_GCM_ERROR_INVALID_ALIGNMENT);
    CHECK_EQ(static_cast<u32>(HleR(m, "cellGcmSys", "cellGcmGetReportDataAddressLocation", {2, hle::CELL_GCM_LOCATION_LOCAL})),
             rsx::kLocalMemoryBase + 0x0FE00000 + 0x20);
}

// Command-buffer writer standing in for libgcm's inlines.
struct Writer {
    Machine& m;
    u32 ctx, io;
    u32 Current() { return m.memory.Read<u32>(ctx + 8); }
    void Word(u32 w) {
        const u32 cur = Current();
        Store(m, cur, w);
        m.memory.Write<u32>(ctx + 8, cur + 4);
    }
    void Method(u32 reg, std::initializer_list<u32> args, bool non_increment = false) {
        Word(rsx::MethodHeader(reg, static_cast<u32>(args.size())) | (non_increment ? rsx::RSX_METHOD_NON_INCREMENT_CMD : 0));
        for (u32 a : args) Word(a);
    }
    void Flush(u32 control) { Store(m, control + 0, Current() - io); }  // cellGcmFlush
};

void TestFifo() {
    std::printf("-- RSX FIFO: reference, semaphores, call/return, methods to the back end, flips, wrap\n");
    Watchdog dog("RSX FIFO");
    Machine m;
    hle::CellGcmSys gcm(*m.kernel);
    gcm.Register(*m.hle);
    const u32 io = MapIoArea(m, 0x100000);
    CHECK_EQ(HleR(m, "cellGcmSys", "_cellGcmInitBody", {kCtxPtr, 0x10000, 0x100000, io}), CELL_OK);
    const u32 ctx = m.memory.Read<u32>(kCtxPtr), control = gcm.control_ea(), labels = gcm.label_ea();
    Writer w{m, ctx, io};
    auto idle = [&] { return Load(m, control + 4) == Load(m, control + 0); };

    // Subroutine at io+0x7000 for CALL/RETURN.
    Store(m, io + 0x7000, rsx::MethodHeader(rsx::NV406E_SET_REFERENCE, 1));
    Store(m, io + 0x7004, 0x2222);
    Store(m, io + 0x7008, rsx::RSX_METHOD_RETURN_CMD);

    w.Word(0);  // NOP
    w.Method(rsx::NV406E_SET_REFERENCE, {0x1111});
    w.Method(rsx::NV406E_SEMAPHORE_OFFSET, {0x20});
    w.Method(rsx::NV406E_SEMAPHORE_RELEASE, {0xCAFE});
    w.Method(rsx::NV4097_SET_SEMAPHORE_OFFSET, {0x30});
    w.Method(rsx::NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE, {0x11223344});
    w.Method(0x1D90, {1, 2});                    // increments: 0x1D90, 0x1D94
    w.Method(0x1808, {5, 6, 7}, true);           // non-increment: 3 x 0x1808
    w.Word(0x7000 | rsx::RSX_METHOD_CALL_CMD);   // call the subroutine
    w.Method(0x0100, {0});                       // NV4097_NO_OPERATION goes to the back end too
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmResetFlipStatus", {}), CELL_OK);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmGetFlipStatus", {}), rsx::CELL_GCM_DISPLAY_FLIP_STATUS_WAITING);
    CHECK_EQ(HleR(m, "cellGcmSys", "_cellGcmSetFlipCommandWithWaitLabel", {ctx, 1, 5, 0x77}), CELL_OK);
    w.Flush(control);
    CHECK(WaitFor(idle));
    CHECK_EQ(Load(m, control + 8), 0x2222);  // the subroutine ran after 0x1111
    CHECK_EQ(Load(m, labels + 0x20), 0xCAFE);
    CHECK_EQ(Load(m, labels + 0x30), 0x11443322);
    CHECK_EQ(Load(m, labels + 0x50), 0x77);
    CHECK_EQ(gcm.backend().count(0x1D90), 1);
    CHECK_EQ(gcm.backend().count(0x1D94), 1);
    CHECK_EQ(gcm.backend().count(0x1808), 3);
    CHECK_EQ(gcm.backend().count(0x0100), 1);
    CHECK_EQ(gcm.backend().flips(), 1);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmGetFlipStatus", {}), rsx::CELL_GCM_DISPLAY_FLIP_STATUS_DONE);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmGetCurrentDisplayBufferId", {kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u8>(kOut), 1);

    // Semaphore acquire blocks the RSX until the PPU writes the label (cellGcmSetWaitLabel).
    w.Method(rsx::NV406E_SEMAPHORE_OFFSET, {0x40});
    w.Method(rsx::NV406E_SEMAPHORE_ACQUIRE, {0x99});
    w.Method(rsx::NV406E_SET_REFERENCE, {0x4444});
    w.Flush(control);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(Load(m, control + 8) != 0x4444);
    Store(m, labels + 0x40, 0x99);
    CHECK(WaitFor([&] { return Load(m, control + 8) == 0x4444; }));

    // Segment wrap through the context callback (what libgcm calls when a segment is full).
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmCallback", {ctx, 16}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(ctx + 0), io + 0x8000);
    CHECK_EQ(m.memory.Read<u32>(ctx + 4), io + 0x10000 - 4);
    CHECK_EQ(w.Current(), io + 0x8000);
    CHECK(WaitFor([&] { return Load(m, control + 4) == 0x8000; }));  // followed the jump
    w.Method(rsx::NV406E_SET_REFERENCE, {0x5555});
    w.Flush(control);
    CHECK(WaitFor([&] { return Load(m, control + 8) == 0x5555; }));
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmCallback", {ctx, 16}), CELL_OK);  // last segment -> back to the first
    CHECK_EQ(w.Current(), io + 0x1000);
    w.Method(rsx::NV406E_SET_REFERENCE, {0x6666});
    w.Flush(control);
    CHECK(WaitFor([&] { return Load(m, control + 8) == 0x6666; }));
    // A flip that does not fit the segment wraps by itself: fill the segment with NOPs first.
    while (w.Current() + 8 <= m.memory.Read<u32>(ctx + 4)) w.Word(0);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetFlipCommand", {ctx, 0}), CELL_OK);
    CHECK_EQ(w.Current(), io + 0x8000 + 8);
    w.Flush(control);
    CHECK(WaitFor([&] { return gcm.backend().flips() == 2; }));
    CHECK(WaitFor(idle));
    CHECK(!m.kernel->is_terminating());
    CHECK(gcm.Describe().size() >= 2);
}

void TestFifoFault() {
    std::printf("-- RSX FIFO: jump to an unmapped IO offset ends the process with a report\n");
    Watchdog dog("RSX fault");
    Machine m;
    hle::CellGcmSys gcm(*m.kernel);
    gcm.Register(*m.hle);
    const u32 io = MapIoArea(m, 0x100000);
    CHECK_EQ(HleR(m, "cellGcmSys", "_cellGcmInitBody", {kCtxPtr, 0x10000, 0x100000, io}), CELL_OK);
    Writer w{m, m.memory.Read<u32>(kCtxPtr), io};
    w.Word(rsx::RSX_METHOD_OLD_JUMP_CMD | 0x0F000000);
    w.Flush(gcm.control_ea());
    CHECK(WaitFor([&] { return m.kernel->is_terminating(); }));
    const auto end = m.kernel->process_end();
    CHECK(end.kind == ppu::kExitAbort);
    CHECK(end.message.find("not mapped in IO space") != std::string::npos);
    CHECK(end.message.find("0x0f000000") != std::string::npos);
}

// The MGS4 boot path at 0x000e4eb4..0x002ed878, as guest code on the interpreter:
// sys_mmapper_allocate_address (syscall 330) -> sys_mmapper_allocate_memory -> map_memory ->
// _cellGcmInitBody(&gCellGcmCurrentContext, ...) -> the libgcm inline that crashed:
//   lwz r9,0(r11)  (context pointer)    lwz r3,8(r9)  (context->current)
void TestGameInitSequence() {
    std::printf("-- PPU regression: mmapper + _cellGcmInitBody, then the inline that faulted at 0x002ed878\n");
    using namespace as;
    Machine m;
    hle::CellGcmSys gcm(*m.kernel);
    gcm.Register(*m.hle);
    constexpr s32 kSlotAlloc = 0x300, kSlotMap = 0x304, kSlotInit = 0x308;
    m.memory.Write<u32>(kData + kSlotAlloc, m.ImportOpd("sysPrxForUser", "sys_mmapper_allocate_memory"));
    m.memory.Write<u32>(kData + kSlotMap, m.ImportOpd("sysPrxForUser", "sys_mmapper_map_memory"));
    m.memory.Write<u32>(kData + kSlotInit, m.ImportOpd("cellGcmSys", "_cellGcmInitBody"));
    Program p;
    p << mflr(31) << lis(30, 2);  // r30 = kData
    p << lis(3, 0x1000) << li(4, 0x400) << lis(5, 0x1000) << addi(6, 30, 0x100);
    p.syscall(330);
    p << mr(21, 3) << lis(3, 0x10) << li(4, 0x400) << addi(5, 30, 0x104);
    p.call_import(30, kSlotAlloc);
    p << mr(22, 3) << lwz(3, 0x100, 30) << lwz(4, 0x104, 30) << lis(5, 4);
    p.call_import(30, kSlotMap);
    p << mr(23, 3) << addi(3, 30, 0x200) << lis(4, 1) << lis(5, 0x10) << lwz(6, 0x100, 30);
    p.call_import(30, kSlotInit);
    p << mr(24, 3) << addi(11, 30, 0x200) << lwz(9, 0, 11) << lwz(20, 8, 9) << mtlr(31) << blr();
    const auto stop = m.Run(p.words(), 1000);
    CHECK(stop.reason == ppu::StopReason::Exited);
    CHECK_EQ(m.ctx.gpr[21], 0);
    CHECK_EQ(m.ctx.gpr[22], 0);
    CHECK_EQ(m.ctx.gpr[23], 0);
    CHECK_EQ(m.ctx.gpr[24], 0);
    const u32 io = m.memory.Read<u32>(kData + 0x100);
    CHECK(m.ctx.gpr[9] != 0);
    CHECK_EQ(m.ctx.gpr[20], io + 0x1000);  // context->current = start of the command buffer
}

// Phase 7.5: NID 0xACEE8542 from the GT5 log (LR 0x00ce7bdc, r3 = 1).
void TestFlipImmediate() {
    std::printf("-- cellGcmSetFlipImmediate (0xACEE8542): argument checks, display state via the RSX thread\n");
    CHECK_EQ(hle::ComputeFnid("cellGcmSetFlipImmediate"), 0xACEE8542u);
    Watchdog dog("flip immediate");
    Machine m;
    hle::CellGcmSys gcm(*m.kernel);
    gcm.Register(*m.hle);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetFlipImmediate", {1}), hle::CELL_GCM_ERROR_FAILURE);  // before init
    const u32 io = MapIoArea(m, 0x100000);
    CHECK_EQ(HleR(m, "cellGcmSys", "_cellGcmInitBody", {kCtxPtr, 0x10000, 0x100000, io}), CELL_OK);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetFlipImmediate", {8}), hle::CELL_GCM_ERROR_FAILURE);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmResetFlipStatus", {}), CELL_OK);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmGetFlipStatus", {}), rsx::CELL_GCM_DISPLAY_FLIP_STATUS_WAITING);
    const u64 flips = gcm.fifo().flip_count.load();
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetFlipImmediate", {0x101}), CELL_OK);  // u8 argument: id 1
    CHECK(WaitFor([&] { return gcm.fifo().flip_count.load() == flips + 1; }));
    CHECK_EQ(gcm.fifo().current_buffer.load(), 1);
    CHECK_EQ(gcm.fifo().immediate_flips(), 1);
    CHECK_EQ(gcm.backend().flips(), 1);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmGetFlipStatus", {}), rsx::CELL_GCM_DISPLAY_FLIP_STATUS_DONE);
    CHECK(m.hle->At(m.hle->Bind("cellGcmSys", hle::ComputeFnid("cellGcmSetFlipImmediate"))).implemented);
}

// ---------------------------------------------------------------------------------------
// Phase 7.7: GCM interrupt handlers on the guest interrupt thread, RSX counters
// ---------------------------------------------------------------------------------------

// Guest handler i (void handler(u32)): bumps a global sequence at kData+0x600 and records, in
// its own slot at kData+0x610+16*i: {sequence of its last call, call count, last argument}.
constexpr u32 kSeq = 0x600;
constexpr u32 Slot(u32 i) { return 0x610 + 16 * i; }
u32 HandlerOpd(Machine& m, u32 i) {
    using namespace as;
    const u32 code = kCode + 0x100 * i, slot = Slot(i);
    const u32 words[] = {lis(5, 2), lwz(6, kSeq, 5), addi(6, 6, 1), stw(6, kSeq, 5), stw(6, static_cast<s32>(slot), 5),
                         lwz(7, static_cast<s32>(slot + 4), 5), addi(7, 7, 1), stw(7, static_cast<s32>(slot + 4), 5),
                         stw(3, static_cast<s32>(slot + 8), 5), blr()};
    for (u32 k = 0; k < std::size(words); ++k) m.memory.Write<u32>(code + 4 * k, words[k]);
    const u32 opd = kData + 0x700 + 8 * i;
    m.memory.Write<u32>(opd, code);
    m.memory.Write<u32>(opd + 4, 0);
    return opd;
}
u32 Calls(Machine& m, u32 i) { return Load(m, kData + Slot(i) + 4); }
u32 LastSeq(Machine& m, u32 i) { return Load(m, kData + Slot(i)); }
u32 LastArg(Machine& m, u32 i) { return Load(m, kData + Slot(i) + 8); }

bool HasThread(Machine& m, const char* name) {
    for (const auto& t : m.kernel->threads().Snapshot())
        if (t.name == name && t.state != lv2::PpuThread::State::Exited) return true;
    return false;
}

void TestInterruptTrampoline() {
    std::printf("-- _gcm_intr_thread trampoline encoding\n");
    const auto w = hle::GcmInterrupts::Trampoline(0x00123450);
    const u32 expected[] = {0x7FE802A6, 0x3D800012, 0x618C3450, 0x7D8903A6, 0x4E800421, 0x2C030000, 0x4182001C, 0x80030000,
                            0x80430004, 0x7C0903A6, 0x7C832378, 0x4E800421, 0x4BFFFFD4, 0x38600000, 0x7FE803A6, 0x4E800020};
    for (std::size_t i = 0; i < std::size(expected); ++i) CHECK_EQ(w[i], expected[i]);
    CHECK_EQ(hle::GcmInterrupts::VBlankIndex(hle::GcmInterrupts::VBlankTime(7)), 7);
    CHECK_EQ(hle::GcmInterrupts::VBlankIndex(hle::GcmInterrupts::VBlankTime(7) - 1), 6);
}

void TestInterruptHandlers() {
    std::printf("-- GCM handlers: vblank / queue / flip / user delivered on _gcm_intr_thread; disable with 0\n");
    Watchdog dog("gcm interrupts");
    Machine m(true);
    hle::CellGcmSys gcm(*m.kernel);
    gcm.Register(*m.hle);
    const u32 vblank = HandlerOpd(m, 0), flip = HandlerOpd(m, 1), queue = HandlerOpd(m, 2), user = HandlerOpd(m, 3);
    const u32 io = MapIoArea(m, 0x100000);
    CHECK(!HasThread(m, "_gcm_intr_thread"));
    CHECK_EQ(HleR(m, "cellGcmSys", "_cellGcmInitBody", {kCtxPtr, 0x10000, 0x100000, io}), CELL_OK);
    CHECK(gcm.interrupts().thread_id() != 0);
    CHECK(HasThread(m, "_gcm_intr_thread"));
    const u32 ctx = m.memory.Read<u32>(kCtxPtr), control = gcm.control_ea();
    Writer w{m, ctx, io};
    auto idle = [&] { return Load(m, control + 4) == Load(m, control + 0); };

    // vblank: 60 Hz, argument 1 (head), on the interrupt thread.
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetVBlankHandler", {vblank}), CELL_OK);
    CHECK(WaitFor([&] { return Calls(m, 0) >= 3; }, 2000));
    CHECK_EQ(LastArg(m, 0), 1);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetVBlankHandler", {0}), CELL_OK);
    const u32 after_disable = Calls(m, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));  // ~7 vblanks
    CHECK(Calls(m, 0) <= after_disable + 1);                       // at most one already in flight

    // FIFO flip: queue handler when the RSX reaches the flip command, flip handler after it.
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetFlipHandler", {flip}), CELL_OK);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetQueueHandler", {queue}), CELL_OK);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmResetFlipStatus", {}), CELL_OK);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetFlip", {ctx, 1}), CELL_OK);
    CHECK_EQ(gcm.flip_commands_written(), 1);
    CHECK(Calls(m, 1) == 0 && Calls(m, 2) == 0);  // nothing before the RSX reaches it
    w.Flush(control);
    CHECK(WaitFor([&] { return Calls(m, 1) == 1 && Calls(m, 2) == 1; }));
    CHECK(LastSeq(m, 2) < LastSeq(m, 1));  // queue, then flip
    CHECK_EQ(LastArg(m, 1), 1);
    CHECK_EQ(LastArg(m, 2), 1);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmGetFlipStatus", {}), rsx::CELL_GCM_DISPLAY_FLIP_STATUS_DONE);
    // Immediate flip: a flip, but no command reached the FIFO -> flip handler only.
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetFlipImmediate", {0}), CELL_OK);
    CHECK(WaitFor([&] { return Calls(m, 1) == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(Calls(m, 2), 1);

    // User command: argument = cause.
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetUserHandler", {user}), CELL_OK);
    w.Method(rsx::kGcmSetUserCommand, {0x1234});
    w.Flush(control);
    CHECK(WaitFor([&] { return Calls(m, 3) == 1; }));
    CHECK_EQ(LastArg(m, 3), 0x1234);

    // Disabled handler: the event is counted, nothing is called.
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetFlipHandler", {0}), CELL_OK);
    CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetFlipImmediate", {2}), CELL_OK);
    CHECK(WaitFor([&] { return gcm.fifo().current_buffer.load() == 2; }));
    CHECK(WaitFor([&] { return gcm.interrupts().counters().no_handler[1] == 1; }));
    CHECK_EQ(Calls(m, 1), 2);

    // Draw / clear counters: decoded, not rendered.
    w.Method(rsx::NV4097_SET_BEGIN_END, {5});
    w.Method(rsx::NV4097_DRAW_ARRAYS, {(2u << 24) | 0});
    w.Method(rsx::NV4097_SET_BEGIN_END, {0});
    w.Method(rsx::NV4097_CLEAR_SURFACE, {0xF3});
    w.Flush(control);
    CHECK(WaitFor(idle));
    const rsx::RsxStats st = gcm.fifo().Stats();
    CHECK_EQ(st.draw_begins, 1);
    CHECK_EQ(st.draw_ends, 1);
    CHECK_EQ(st.draw_arrays, 1);
    CHECK_EQ(st.clears, 1);
    CHECK_EQ(st.fifo_flips, 1);
    CHECK_EQ(st.immediate_flips, 2);
    CHECK_EQ(st.user_commands, 1);
    CHECK_EQ(st.frames_presented, 0);
    CHECK(st.put_advances >= 3);
    CHECK_EQ(st.put, st.get);
    const auto c = gcm.interrupts().counters();
    CHECK_EQ(c.delivered[1], 2);
    CHECK_EQ(c.delivered[2], 1);
    CHECK_EQ(c.delivered[3], 1);
    bool described = false;
    for (const std::string& line : gcm.Describe())
        if (line.find("frames presented to a host surface: 0") != std::string::npos) described = true;
    CHECK(described);
}

void TestInterruptLifecycle() {
    std::printf("-- _gcm_intr_thread: off with --no-gcm-interrupts; exits when cellGcm goes away / on termination\n");
    Watchdog dog("gcm interrupt lifecycle");
    {
        Machine m;
        hle::CellGcmSys gcm(*m.kernel);
        gcm.set_interrupts_enabled(false);
        gcm.Register(*m.hle);
        const u32 io = MapIoArea(m, 0x100000);
        CHECK_EQ(HleR(m, "cellGcmSys", "_cellGcmInitBody", {kCtxPtr, 0x10000, 0x100000, io}), CELL_OK);
        CHECK_EQ(gcm.interrupts().thread_id(), 0);
        CHECK(!HasThread(m, "_gcm_intr_thread"));
        CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetVBlankHandler", {0x1234}), CELL_OK);  // recorded only
    }
    {
        Machine m(true);
        const u32 vblank = HandlerOpd(m, 0);
        {
            hle::CellGcmSys gcm(*m.kernel);
            gcm.Register(*m.hle);
            const u32 io = MapIoArea(m, 0x100000);
            CHECK_EQ(HleR(m, "cellGcmSys", "_cellGcmInitBody", {kCtxPtr, 0x10000, 0x100000, io}), CELL_OK);
            CHECK_EQ(HleR(m, "cellGcmSys", "cellGcmSetVBlankHandler", {vblank}), CELL_OK);
            CHECK(WaitFor([&] { return Calls(m, 0) >= 1; }, 2000));
        }
        // CellGcmSys gone: the thread leaves its loop and exits (detached: reaped at once).
        CHECK(WaitFor([&] { return !HasThread(m, "_gcm_intr_thread"); }));
        CHECK(!m.kernel->is_terminating());
    }
    {
        // Process termination while the thread waits: it stops, Shutdown() joins it.
        Machine m;
        hle::CellGcmSys gcm(*m.kernel);
        gcm.Register(*m.hle);
        const u32 io = MapIoArea(m, 0x100000);
        CHECK_EQ(HleR(m, "cellGcmSys", "_cellGcmInitBody", {kCtxPtr, 0x10000, 0x100000, io}), CELL_OK);
        CHECK(WaitFor([&] {
            for (const auto& t : m.kernel->threads().Snapshot())
                if (t.name == "_gcm_intr_thread" && t.live_kind == ppu::PpuLiveStatus::kInHle) return true;
            return false;
        }));
        m.kernel->Shutdown();
        CHECK(!HasThread(m, "_gcm_intr_thread"));
    }
}

}  // namespace

int main() {
    log::Threshold() = log::Level::Error;
    TestMmapper();
    TestGcmInitAndIo();
    TestFifo();
    TestFifoFault();
    TestGameInitSequence();
    TestFlipImmediate();
    TestInterruptTrampoline();
    TestInterruptHandlers();
    TestInterruptLifecycle();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all GCM tests passed\n");
    return 0;
}
