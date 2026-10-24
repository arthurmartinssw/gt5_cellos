// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/audio_tests.cpp
//
// Phase 7.4: cellAudio and the stubs from the GT5 log.
//   * port lifecycle and error codes (init twice, open/start/stop/close, PORT_FULL, PARAM);
//   * CellAudioPortParam / CellAudioPortConfig byte layout, and that readIndexAddr and portAddr
//     point into mapped guest memory of the right size (the GT5 crash read through them);
//   * the mixer period: block consumed and cleared, big-endian read index advanced and
//     wrapping, tags and timestamps, a stopped port frozen;
//   * notify queues: cellAudioCreateNotifyEventQueue, a queue made by the game with its own key,
//     one event per period; unknown key -> TRANS_EVENT;
//   * the real-time mixer thread delivers events at ~5.33 ms;
//   * FNIDs of the four unknown imports, cellAudioOutGetDeviceInfo layout, cellUsbd accepted.
#include <bit>
#include <chrono>
#include <thread>

#include "hle/cell_audio.h"
#include "hle/cell_sysutil.h"
#include "hle/cell_usbd.h"
#include "hle/fnid.h"
#include "lv2/sys_event.h"
#include "test_machine.h"

using namespace cell;

namespace {

constexpr u32 kParam = kData + 0x100, kOut = kData + 0x200, kCfg = kData + 0x300;

s32 Call(Machine& m, const char* module, const char* name, std::initializer_list<u64> args, double f1 = 0.0) {
    const u32 index = m.hle->Bind(module, hle::ComputeFnid(name));
    const hle::HleFunction& f = m.hle->At(index);
    CHECK(f.implemented);
    ppu::PpuThreadContext c{};
    int r = 3;
    for (u64 a : args) c.gpr[r++] = a;
    c.fpr[1] = f1;
    f.fn(c, m.memory, f);
    return static_cast<s32>(c.gpr[3]);
}

s32 Sys(Machine& m, u32 n, std::initializer_list<u64> args, ppu::PpuThreadContext* out = nullptr) {
    ppu::PpuThreadContext c{};
    c.gpr[11] = n;
    int r = 3;
    for (u64 a : args) c.gpr[r++] = a;
    m.syscalls.Dispatch(c);
    if (out) *out = c;
    return static_cast<s32>(c.gpr[3]);
}

void WriteParam(Machine& m, u64 channels, u64 blocks, u64 attr = 0, float level = 1.0f) {
    m.memory.Write<u64>(kParam + 0, channels);
    m.memory.Write<u64>(kParam + 8, blocks);
    m.memory.Write<u64>(kParam + 16, attr);
    m.memory.Write<u32>(kParam + 24, std::bit_cast<u32>(level));
}

s32 Open(Machine& m, u64 channels, u64 blocks) {
    WriteParam(m, channels, blocks);
    const s32 r = Call(m, "cellAudio", "cellAudioPortOpen", {kParam, kOut});
    return r == CELL_OK ? static_cast<s32>(m.memory.Read<u32>(kOut)) : r;
}

void TestNids() {
    std::printf("-- FNIDs of the unknown imports in the GT5 log\n");
    CHECK_EQ(hle::ComputeFnid("cellUsbdInit"), 0xD0E766FEu);
    CHECK_EQ(hle::ComputeFnid("cellUsbdSetThreadPriority2"), 0x5C832BD7u);
    CHECK_EQ(hle::ComputeFnid("cellAudioOutGetDeviceInfo"), 0x7663E368u);
    CHECK_EQ(hle::ComputeFnid("cellAudioOutSetCopyControl"), 0xC96E89E9u);
}

void TestPortLifecycle() {
    std::printf("-- cellAudio: init, port open/config/start/stop/close, error codes, structure layout\n");
    Machine m;
    hle::CellAudio audio(*m.kernel, false);
    audio.Register(*m.hle);
    CHECK_EQ(Open(m, 2, 8), hle::CELL_AUDIO_ERROR_NOT_INIT);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioInit", {}), CELL_OK);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioInit", {}), hle::CELL_AUDIO_ERROR_ALREADY_INIT);
    CHECK_EQ(Open(m, 6, 8), hle::CELL_AUDIO_ERROR_PARAM);
    CHECK_EQ(Open(m, 2, 3), hle::CELL_AUDIO_ERROR_PARAM);
    const s32 port = Open(m, 2, 8);
    CHECK_EQ(port, 0);

    m.memory.Fill(kCfg, 0xEE, 40);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioGetPortConfig", {static_cast<u64>(port), kCfg}), CELL_OK);
    const u32 index_addr = m.memory.Read<u32>(kCfg + 0);
    const u32 size = m.memory.Read<u32>(kCfg + 24), addr = m.memory.Read<u32>(kCfg + 28);
    CHECK_EQ(m.memory.Read<u32>(kCfg + 4), hle::CELL_AUDIO_STATUS_READY);
    CHECK_EQ(m.memory.Read<u64>(kCfg + 8), 2);
    CHECK_EQ(m.memory.Read<u64>(kCfg + 16), 8);
    CHECK_EQ(size, 2 * 8 * 256 * 4);
    CHECK_EQ(m.memory.Read<u8>(kCfg + 32), 0xEE);  // 32 bytes only
    CHECK(index_addr && m.memory.Check(index_addr, 8, vm::kPageReadable | vm::kPageWritable));
    CHECK(addr && m.memory.Check(addr, size, vm::kPageReadable | vm::kPageWritable));
    CHECK_EQ(m.memory.Read<u64>(index_addr), 0);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioGetPortConfig", {8, kCfg}), hle::CELL_AUDIO_ERROR_PARAM);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioGetPortConfig", {1, kCfg}), CELL_OK);  // never opened
    CHECK_EQ(m.memory.Read<u32>(kCfg + 4), hle::CELL_AUDIO_STATUS_CLOSE);

    CHECK_EQ(Call(m, "cellAudio", "cellAudioPortStop", {0}), hle::CELL_AUDIO_ERROR_PORT_NOT_RUN);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioPortStart", {0}), CELL_OK);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioPortStart", {0}), hle::CELL_AUDIO_ERROR_PORT_ALREADY_RUN);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioPortStart", {1}), hle::CELL_AUDIO_ERROR_PORT_NOT_OPEN);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioGetPortConfig", {0, kCfg}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kCfg + 4), hle::CELL_AUDIO_STATUS_RUN);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioSetPortLevel", {0}, 0.5), CELL_OK);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioSetPortLevel", {0}, -1.0), hle::CELL_AUDIO_ERROR_PARAM);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioAddData", {0, kData, 256}, 1.0), CELL_OK);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioAddData", {3, kData, 256}, 1.0), hle::CELL_AUDIO_ERROR_PORT_NOT_OPEN);

    // all 8 ports
    for (int i = 1; i < 8; ++i) CHECK_EQ(Open(m, 8, 32), i);
    CHECK_EQ(Open(m, 2, 2), hle::CELL_AUDIO_ERROR_PORT_FULL);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioPortClose", {7}), CELL_OK);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioPortClose", {7}), hle::CELL_AUDIO_ERROR_PORT_NOT_OPEN);
    CHECK_EQ(Open(m, 2, 2), 7);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioQuit", {}), CELL_OK);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioQuit", {}), hle::CELL_AUDIO_ERROR_NOT_INIT);
}

void TestMixerPeriod() {
    std::printf("-- mixer period: block cleared, read index advances and wraps, tags, timestamps, stop\n");
    // Timestamps count from the first use of the guest clock; leave room for "3 periods earlier".
    (void)ppu::PpuInterpreter::SystemTimeMicroseconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Machine m;
    hle::CellAudio audio(*m.kernel, false);
    audio.Register(*m.hle);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioInit", {}), CELL_OK);
    const s32 port = Open(m, 2, 4);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioGetPortConfig", {static_cast<u64>(port), kCfg}), CELL_OK);
    const u32 index_addr = m.memory.Read<u32>(kCfg), addr = m.memory.Read<u32>(kCfg + 28);
    constexpr u32 kBlock = 2 * 256 * 4;
    m.memory.Fill(addr, 0x3F, 4 * kBlock);  // the game filled every block
    audio.Tick();                            // not started: nothing moves
    CHECK_EQ(m.memory.Read<u64>(index_addr), 0);
    CHECK_EQ(m.memory.Read<u8>(addr), 0x3F);

    CHECK_EQ(Call(m, "cellAudio", "cellAudioPortStart", {static_cast<u64>(port)}), CELL_OK);
    audio.Tick();
    CHECK_EQ(m.memory.Read<u64>(index_addr), 1);
    CHECK_EQ(m.memory.Read<u8>(addr), 0);                    // block 0 played -> silence
    CHECK_EQ(m.memory.Read<u8>(addr + kBlock - 1), 0);
    CHECK_EQ(m.memory.Read<u8>(addr + kBlock), 0x3F);        // block 1 untouched
    for (int i = 0; i < 4; ++i) audio.Tick();
    CHECK_EQ(m.memory.Read<u64>(index_addr), 1);             // 5 periods, 4 blocks: wrapped

    // tags: the current block is due at tag 5; block 0 at tag 8
    CHECK_EQ(Call(m, "cellAudio", "cellAudioGetPortBlockTag", {static_cast<u64>(port), 1, kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u64>(kOut), 5);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioGetPortBlockTag", {static_cast<u64>(port), 0, kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u64>(kOut), 8);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioGetPortBlockTag", {static_cast<u64>(port), 4, kOut}), hle::CELL_AUDIO_ERROR_PARAM);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioGetPortTimestamp", {static_cast<u64>(port), 5, kOut}), CELL_OK);
    const u64 t5 = m.memory.Read<u64>(kOut);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioGetPortTimestamp", {static_cast<u64>(port), 2, kOut}), CELL_OK);
    CHECK_EQ(t5 - m.memory.Read<u64>(kOut), 3 * hle::kAudioPeriodNs / 1000);  // 3 periods earlier
    CHECK_EQ(Call(m, "cellAudio", "cellAudioGetPortTimestamp", {static_cast<u64>(port), 6, kOut}), hle::CELL_AUDIO_ERROR_TAG_NOT_FOUND);

    CHECK_EQ(Call(m, "cellAudio", "cellAudioPortStop", {static_cast<u64>(port)}), CELL_OK);
    audio.Tick();
    CHECK_EQ(m.memory.Read<u64>(index_addr), 1);  // frozen while stopped
}

void TestNotifyQueues() {
    std::printf("-- notify queues: created by cellAudio or by the game, one event per period\n");
    Machine m;
    hle::CellAudio audio(*m.kernel, false);
    audio.Register(*m.hle);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioInit", {}), CELL_OK);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioCreateNotifyEventQueue", {kOut, kOut + 8}), CELL_OK);
    const u32 q1 = m.memory.Read<u32>(kOut);
    const u64 key1 = m.memory.Read<u64>(kOut + 8);
    CHECK_EQ(key1, hle::kAudioNotifyKeyBase);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioSetNotifyEventQueue", {key1}), CELL_OK);

    // the game's own queue (sys_event_queue_create with an IPC key)
    m.memory.Write<u32>(kData + 0x400, lv2::SYS_SYNC_FIFO);
    m.memory.Write<u32>(kData + 0x404, lv2::SYS_PPU_QUEUE);
    m.memory.Write<u64>(kData + 0x408, 0);
    CHECK_EQ(Sys(m, 128, {kData + 0x410, kData + 0x400, 0x1234567800000001ull, 8}), CELL_OK);
    const u32 q2 = m.memory.Read<u32>(kData + 0x410);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioSetNotifyEventQueueEx", {0x1234567800000001ull, 0}), CELL_OK);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioSetNotifyEventQueue", {0x99}), hle::CELL_AUDIO_ERROR_TRANS_EVENT);

    CHECK_EQ(Sys(m, 131, {q1, kOut, 1, kOut + 0x40}), CELL_OK);  // tryreceive: empty
    CHECK_EQ(m.memory.Read<u32>(kOut + 0x40), 0);
    audio.Tick();
    audio.Tick();
    ppu::PpuThreadContext c{};
    CHECK_EQ(Sys(m, 130, {q1, 0, 0}, &c), CELL_OK);
    CHECK_EQ(Sys(m, 130, {q1, 0, 0}, &c), CELL_OK);
    CHECK_EQ(Sys(m, 130, {q2, 0, 0}, &c), CELL_OK);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioRemoveNotifyEventQueue", {key1}), CELL_OK);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioRemoveNotifyEventQueue", {key1}), hle::CELL_AUDIO_ERROR_TRANS_EVENT);
    audio.Tick();
    CHECK_EQ(Sys(m, 130, {q1, 0, 1000}, &c), CELL_ETIMEDOUT);  // removed: no more events
    CHECK_EQ(Sys(m, 130, {q2, 0, 0}, &c), CELL_OK);
}

void TestRealtimeMixer() {
    std::printf("-- real-time mixer: events every ~5.33 ms\n");
    Machine m;
    hle::CellAudio audio(*m.kernel, true);
    audio.Register(*m.hle);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioInit", {}), CELL_OK);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioCreateNotifyEventQueue", {kOut, kOut + 8}), CELL_OK);
    const u32 q = m.memory.Read<u32>(kOut);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioSetNotifyEventQueue", {m.memory.Read<u64>(kOut + 8)}), CELL_OK);
    const s32 port = Open(m, 2, 8);
    CHECK_EQ(Call(m, "cellAudio", "cellAudioPortStart", {static_cast<u64>(port)}), CELL_OK);
    const auto start = std::chrono::steady_clock::now();
    ppu::PpuThreadContext c{};
    for (int i = 0; i < 30; ++i) CHECK_EQ(Sys(m, 130, {q, 0, 2'000'000}, &c), CELL_OK);  // 30 periods
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(ms > 100.0 && ms < 2000.0);  // ~160 ms nominal; generous for loaded CI and sanitizers
    CHECK_EQ(Call(m, "cellAudio", "cellAudioQuit", {}), CELL_OK);
}

void TestSysutilAndUsbd() {
    std::printf("-- cellAudioOutGetDeviceInfo / SetCopyControl, cellUsbd accepted\n");
    Machine m;
    hle::CellSysutil su(*m.kernel, {});
    su.Register(*m.hle);
    hle::CellUsbd usbd;
    usbd.Register(*m.hle);
    m.memory.Fill(kOut, 0xEE, 144);
    CHECK_EQ(Call(m, "cellSysutil", "cellAudioOutGetDeviceInfo", {0, 0, kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u8>(kOut + 1), 1);    // one mode
    CHECK_EQ(m.memory.Read<u8>(kOut + 2), 1);    // available
    CHECK_EQ(m.memory.Read<u8>(kOut + 9), 2);    // 2 channels
    CHECK_EQ(m.memory.Read<u8>(kOut + 10), 4);   // 48 kHz
    CHECK_EQ(m.memory.Read<u8>(kOut + 136), 0xEE);
    CHECK_EQ(Call(m, "cellSysutil", "cellAudioOutGetDeviceInfo", {0, 1, kOut}), hle::CELL_AUDIO_OUT_ERROR_DEVICE_NOT_FOUND);
    CHECK_EQ(Call(m, "cellSysutil", "cellAudioOutSetCopyControl", {0, 0}), CELL_OK);
    CHECK_EQ(Call(m, "cellUsbd", "cellUsbdInit", {}), CELL_OK);
    CHECK_EQ(Call(m, "cellUsbd", "cellUsbdSetThreadPriority2", {500, 500, 500}), CELL_OK);
    CHECK_EQ(Call(m, "cellUsbd", "cellUsbdRegisterLdd", {kData}), CELL_OK);
}

}  // namespace

int main() {
    log::Threshold() = log::Level::Error;
    TestNids();
    TestPortLifecycle();
    TestMixerPeriod();
    TestNotifyQueues();
    TestRealtimeMixer();
    TestSysutilAndUsbd();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all audio tests passed\n");
    return 0;
}
