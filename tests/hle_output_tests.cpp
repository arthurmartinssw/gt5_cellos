// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/hle_output_tests.cpp
//
// Phase 7.7: the imports GT5 called before its primary-thread stall that used to be logging
// stubs returning CELL_OK with their outputs unwritten — sys_mempool_create (0xCA9A60BF),
// cellKbInit (0x433F6EC0), cellMouseInit (0xC9030138), cellKey2CharOpen (0xABF629C1) — and the
// lwcond provenance the stall report now prints (creator, past waiters / signalers).
#include <atomic>
#include <chrono>
#include <thread>

#include "hle/cell_pad.h"
#include "hle/fnid.h"
#include "lv2/sys_sync.h"
#include "test_machine.h"

using namespace cell;

namespace {

const hle::HleFunction& Fn(Machine& m, const char* module, const char* name) {
    return m.hle->At(m.hle->Bind(module, hle::ComputeFnid(name)));
}
u64 Call(Machine& m, const char* module, const char* name, std::initializer_list<u64> args, u32 tid = 0, u32 lr = 0) {
    const hle::HleFunction& f = Fn(m, module, name);
    CHECK(f.implemented);
    ppu::PpuThreadContext c{};
    c.thread_id = tid;
    c.lr = lr;
    int i = 3;
    for (u64 a : args) c.gpr[i++] = a;
    f.fn(c, m.memory, f);
    return c.gpr[3];
}
s32 CallR(Machine& m, const char* module, const char* name, std::initializer_list<u64> args, u32 tid = 0, u32 lr = 0) {
    return static_cast<s32>(Call(m, module, name, args, tid, lr));
}

constexpr u32 kOut = kData + 0x100;
constexpr u32 kChunk = kData + 0x1000;  // 8-byte aligned

void TestNids() {
    std::printf("-- the five GT5 imports: FNIDs of the verified names\n");
    CHECK_EQ(hle::ComputeFnid("sys_mempool_create"), 0xCA9A60BFu);
    CHECK_EQ(hle::ComputeFnid("cellKbInit"), 0x433F6EC0u);
    CHECK_EQ(hle::ComputeFnid("cellKey2CharOpen"), 0xABF629C1u);
    CHECK_EQ(hle::ComputeFnid("sys_prx_get_module_list"), 0xA5D06BF0u);
    CHECK_EQ(hle::ComputeFnid("cellMouseInit"), 0xC9030138u);
}

void TestMempool() {
    std::printf("-- sys_mempool: create checks, id written, LIFO blocks, count, blocking allocate, destroy\n");
    Machine m;
    const char* P = "sysPrxForUser";
    // Argument checks (RPCS3 order), nothing written on failure.
    m.memory.Write<u32>(kOut, 0xDEADBEEF);
    CHECK_EQ(CallR(m, P, "sys_mempool_create", {kOut, kChunk, 0x100, 0x200, 0}), CELL_EINVAL);  // block > chunk
    CHECK_EQ(CallR(m, P, "sys_mempool_create", {kOut, kChunk, 0x400, 0x100, 3}), CELL_EINVAL);  // alignment 3
    CHECK_EQ(CallR(m, P, "sys_mempool_create", {kOut, kChunk + 4, 0x400, 0x100, 0}), CELL_EINVAL);  // chunk & 7
    CHECK_EQ(CallR(m, P, "sys_mempool_create", {kOut, kChunk, 0x400, 0, 0}), CELL_EINVAL);  // block size 0
    CHECK_EQ(CallR(m, P, "sys_mempool_create", {0, kChunk, 0x400, 0x100, 0}), CELL_EFAULT);
    CHECK_EQ(m.memory.Read<u32>(kOut), 0xDEADBEEF);

    CHECK_EQ(CallR(m, P, "sys_mempool_create", {kOut, kChunk, 0x400, 0x100, 2}), CELL_OK);  // 2 means 4
    const u32 pool = m.memory.Read<u32>(kOut);
    CHECK(pool != 0 && pool != 0xDEADBEEF);
    CHECK_EQ(Call(m, P, "sys_mempool_get_count", {pool}), 4);
    CHECK_EQ(static_cast<u32>(Call(m, P, "sys_mempool_allocate_block", {pool})), kChunk + 0x300);  // highest first
    CHECK_EQ(static_cast<u32>(Call(m, P, "sys_mempool_try_allocate_block", {pool})), kChunk + 0x200);
    CHECK_EQ(Call(m, P, "sys_mempool_get_count", {pool}), 2);
    CHECK_EQ(CallR(m, P, "sys_mempool_free_block", {pool, kChunk + 0x500}), CELL_EINVAL);  // past the chunk
    CHECK_EQ(CallR(m, P, "sys_mempool_free_block", {pool, kChunk + 0x300}), CELL_OK);
    CHECK_EQ(static_cast<u32>(Call(m, P, "sys_mempool_try_allocate_block", {pool})), kChunk + 0x300);  // LIFO
    CHECK_EQ(static_cast<u32>(Call(m, P, "sys_mempool_try_allocate_block", {pool})), kChunk + 0x100);
    CHECK_EQ(static_cast<u32>(Call(m, P, "sys_mempool_try_allocate_block", {pool})), kChunk + 0x000);
    CHECK_EQ(Call(m, P, "sys_mempool_try_allocate_block", {pool}), 0);  // empty: never waits
    CHECK_EQ(Call(m, P, "sys_mempool_get_count", {pool}), 0);
    CHECK_EQ(static_cast<s32>(Call(m, P, "sys_mempool_get_count", {pool + 100})), CELL_EINVAL);
    CHECK_EQ(Call(m, P, "sys_mempool_allocate_block", {pool + 100}), 0);

    // allocate_block on an empty pool waits until a block is freed.
    std::atomic<u32> got{1};
    std::thread waiter([&] { got = static_cast<u32>(Call(m, P, "sys_mempool_allocate_block", {pool})); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(got.load(), 1);  // still waiting
    CHECK_EQ(CallR(m, P, "sys_mempool_free_block", {pool, kChunk + 0x100}), CELL_OK);
    waiter.join();
    CHECK_EQ(got.load(), kChunk + 0x100);

    // destroy wakes a waiting allocator with 0.
    std::atomic<u32> got2{1};
    std::thread waiter2([&] { got2 = static_cast<u32>(Call(m, P, "sys_mempool_allocate_block", {pool})); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Call(m, P, "sys_mempool_destroy", {pool});
    waiter2.join();
    CHECK_EQ(got2.load(), 0);
    CHECK_EQ(static_cast<s32>(Call(m, P, "sys_mempool_get_count", {pool})), CELL_EINVAL);

    // A waiter blocked at process termination returns 0 (and Shutdown can join it).
    CHECK_EQ(CallR(m, P, "sys_mempool_create", {kOut, kChunk, 0x100, 0x100, 0}), CELL_OK);
    const u32 pool2 = m.memory.Read<u32>(kOut);
    CHECK(Call(m, P, "sys_mempool_allocate_block", {pool2}) != 0);
    std::atomic<u32> got3{1};
    std::thread waiter3([&] { got3 = static_cast<u32>(Call(m, P, "sys_mempool_allocate_block", {pool2})); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    lv2::ProcessEnd end;
    m.kernel->RequestTermination(end);
    waiter3.join();
    CHECK_EQ(got3.load(), 0);
}

void TestKeyboardMouse() {
    std::printf("-- cellKb / cellMouse (sys_io) with no device, cellKey2CharOpen\n");
    Machine m;
    hle::CellPad pad;
    pad.Register(*m.hle);
    // Keyboard
    CHECK_EQ(CallR(m, "sys_io", "cellKbGetInfo", {kOut}), hle::CELL_KB_ERROR_UNINITIALIZED);
    CHECK_EQ(CallR(m, "sys_io", "cellKbEnd", {}), hle::CELL_KB_ERROR_UNINITIALIZED);
    CHECK_EQ(CallR(m, "sys_io", "cellKbInit", {0}), hle::CELL_KB_ERROR_INVALID_PARAMETER);
    CHECK_EQ(CallR(m, "sys_io", "cellKbInit", {128}), hle::CELL_KB_ERROR_INVALID_PARAMETER);
    CHECK_EQ(CallR(m, "sys_io", "cellKbInit", {127}), CELL_OK);
    CHECK_EQ(CallR(m, "sys_io", "cellKbInit", {1}), hle::CELL_KB_ERROR_ALREADY_INITIALIZED);
    m.memory.Fill(kOut, 0xAB, hle::kCellKbInfoSize + 4);
    CHECK_EQ(CallR(m, "sys_io", "cellKbGetInfo", {0}), hle::CELL_KB_ERROR_INVALID_PARAMETER);
    CHECK_EQ(CallR(m, "sys_io", "cellKbGetInfo", {kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut + 0), 7);  // min(127, 7), as RPCS3 stores it
    CHECK_EQ(m.memory.Read<u32>(kOut + 4), 0);  // now_connect
    CHECK_EQ(m.memory.Read<u32>(kOut + 8), 0);  // info
    bool zero = true;
    for (u32 i = 0; i < 127; ++i) zero &= m.memory.Read<u8>(kOut + 12 + i) == 0;
    CHECK(zero);
    CHECK_EQ(m.memory.Read<u8>(kOut + hle::kCellKbInfoSize), 0xAB);  // nothing past the struct
    CHECK_EQ(CallR(m, "sys_io", "cellKbEnd", {}), CELL_OK);
    CHECK_EQ(CallR(m, "sys_io", "cellKbGetInfo", {kOut}), hle::CELL_KB_ERROR_UNINITIALIZED);
    // Mouse
    CHECK_EQ(CallR(m, "sys_io", "cellMouseGetInfo", {kOut}), hle::CELL_MOUSE_ERROR_UNINITIALIZED);
    CHECK_EQ(CallR(m, "sys_io", "cellMouseInit", {0}), hle::CELL_MOUSE_ERROR_INVALID_PARAMETER);
    CHECK_EQ(CallR(m, "sys_io", "cellMouseInit", {2}), CELL_OK);
    CHECK_EQ(CallR(m, "sys_io", "cellMouseInit", {2}), hle::CELL_MOUSE_ERROR_ALREADY_INITIALIZED);
    m.memory.Fill(kOut, 0xAB, hle::kCellMouseInfoSize + 4);
    CHECK_EQ(CallR(m, "sys_io", "cellMouseGetInfo", {kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut + 0), 2);
    zero = true;
    for (u32 i = 4; i < hle::kCellMouseInfoSize; ++i) zero &= m.memory.Read<u8>(kOut + i) == 0;
    CHECK(zero);
    CHECK_EQ(m.memory.Read<u8>(kOut + hle::kCellMouseInfoSize), 0xAB);
    CHECK_EQ(CallR(m, "sys_io", "cellMouseEnd", {}), CELL_OK);
    CHECK_EQ(CallR(m, "sys_io", "cellMouseEnd", {}), hle::CELL_MOUSE_ERROR_UNINITIALIZED);
    // Key2Char: handle checked, not written.
    CHECK_EQ(CallR(m, "cellKey2char", "cellKey2CharOpen", {0}), hle::CELL_K2C_ERROR_INVALID_HANDLE);
    m.memory.Fill(kOut, 0, 128);
    CHECK_EQ(CallR(m, "cellKey2char", "cellKey2CharOpen", {kOut}), CELL_OK);
    m.memory.Write<u8>(kOut + 8, 1);
    CHECK_EQ(CallR(m, "cellKey2char", "cellKey2CharOpen", {kOut}), hle::CELL_K2C_ERROR_ALREADY_INITIALIZED);
}

void TestLwcondProvenance() {
    std::printf("-- lwcond provenance for the stall report: creator, past waiters / signalers, ESRCH on a bad id\n");
    Machine m;
    const char* P = "sysPrxForUser";
    constexpr u32 kMutex = kData + 0x400, kAttr = kData + 0x440, kCond = kData + 0x480;
    m.memory.Write<u32>(kAttr + 0, lv2::SYS_SYNC_PRIORITY);
    m.memory.Write<u32>(kAttr + 4, lv2::SYS_SYNC_NOT_RECURSIVE);
    m.memory.Write<u64>(kAttr + 8, 0x6D61696E5F6D7478ull);  // "main_mtx"
    CHECK_EQ(CallR(m, P, "sys_lwmutex_create", {kMutex, kAttr}), CELL_OK);
    CHECK_EQ(CallR(m, P, "sys_lwcond_create", {kCond, kMutex, kAttr + 8}, 0x01000000, 0x00c03d00), CELL_OK);
    const auto cond = m.kernel->objects().Get<lv2::Lv2Cond>(m.memory.Read<u32>(kCond + 4));
    CHECK(cond != nullptr);
    if (!cond) return;
    CHECK_EQ(cond->creator, 0x01000000);
    CHECK_EQ(cond->creator_lr, 0x00c03d00);
    CHECK(cond->Signalers().empty());
    // A waiter (tid 0x0100000c) woken by a signaler (tid 0x01000014).
    std::atomic<s32> wait_result{1};
    std::thread waiter([&] {
        CHECK_EQ(CallR(m, P, "sys_lwmutex_lock", {kMutex, 0}, 0x0100000c), CELL_OK);
        wait_result = CallR(m, P, "sys_lwcond_wait", {kCond, 0}, 0x0100000c);
        CallR(m, P, "sys_lwmutex_unlock", {kMutex}, 0x0100000c);
    });
    bool waiting = false;
    for (int i = 0; i < 500 && !waiting; ++i) {
        {
            std::lock_guard lock(cond->mutex->m);
            waiting = !cond->waiters.empty();
        }
        if (!waiting) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(waiting);
    CHECK_EQ(CallR(m, P, "sys_lwcond_signal", {kCond}, 0x01000014), CELL_OK);
    waiter.join();
    CHECK_EQ(wait_result.load(), CELL_OK);
    const auto signalers = cond->Signalers();
    CHECK(signalers.size() == 1 && signalers[0] == 0x01000014);
    const std::string stats = cond->Stats();
    CHECK(stats.find("created by 0x01000000 (lr 0x00c03d00)") != std::string::npos);
    CHECK(stats.find("ever waited on by: 0x0100000c") != std::string::npos);
    CHECK(stats.find("ever signaled by: 0x01000014") != std::string::npos);
    // A copy of the struct with a stale id: ESRCH (and a log line), never a crash.
    m.memory.Write<u32>(kCond + 0x40, kMutex);
    m.memory.Write<u32>(kCond + 0x44, 0x970FFFFF);
    CHECK_EQ(CallR(m, P, "sys_lwcond_signal", {kCond + 0x40}), CELL_ESRCH);
}

}  // namespace

int main() {
    log::Threshold() = log::Level::Error;
    TestNids();
    TestMempool();
    TestKeyboardMouse();
    TestLwcondProvenance();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all HLE output tests passed\n");
    return 0;
}
