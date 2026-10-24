// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — common/host_thread.h
//
// Host-OS thread helpers. Everything else in the project uses portable <thread>; only the
// operations C++20 has no API for live here, behind explicit per-OS branches:
//   * naming the calling thread, so PPU/SPU threads show up as "PPU 0x01000001" / "SPU 0x...
//     " in debuggers, profilers (perf, VTune, PIX) and crash dumps.
//   * sleeping for less than a Windows scheduler tick. std::this_thread::sleep_for is not
//     usable for the RSX/SPU polling loops on Windows: MinGW-w64 (winpthreads) truncates
//     sub-millisecond sleeps to Sleep(0) (a busy yield), and Sleep(1) lasts a full timer tick
//     (15.6 ms by default) — about one frame of latency per wake-up.
#pragma once

#include <cstdint>

namespace cell::host {

// Names the calling thread. Best effort: silently ignored where unsupported.
void SetCurrentThreadName(const char* name) noexcept;

// Sleeps for about `microseconds` (never less than requested where the OS allows it).
//   Windows 10 1803+: per-thread high-resolution waitable timer (~0.5 ms precision);
//                     older Windows: Sleep() rounded up to whole milliseconds.
//   Linux:            clock_nanosleep (tens of microseconds of slack).
// 0 yields the CPU.
void SleepMicroseconds(std::uint32_t microseconds) noexcept;

}  // namespace cell::host
