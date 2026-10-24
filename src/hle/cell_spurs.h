// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_spurs.h
//
// cellSpurs (libsre): the PS3's SPU task scheduler. On hardware, cellSpursInitialize creates
// an SPU thread group running the SPURS *kernel* (SPU code shipped in the system PRX) plus a
// PPU handler thread; workloads (tasksets, job chains, custom policy modules) are SPU programs
// that kernel schedules on the SPUs.
//
// Translation-layer plan (phases 5+): SPURS is not emulated as SPU code. Its kernel becomes a
// host scheduler: each SPURS instance owns a pool of host worker threads (one per SPU it was
// given) that pick workloads by priority/contention and run their SPU code through the SPU
// executor/JIT, with the task/job/policy-module ABI (LS layout, DMA of task contexts, kernel
// mailbox protocol) implemented in C++.
//
// Phase 4 (this file): the attribute/initialize/finalize surface is accepted and tracked
// host-side, so a game gets through boot-time setup; the first call that hands SPURS real SPU
// work (taskset / task / job chain / workload) ends the process with a clear report naming the
// call, instead of returning CELL_OK for work that would never run.
#pragma once

#include <mutex>
#include <set>

#include "hle/hle_registry.h"

namespace cell::hle {

inline constexpr s32 CELL_SPURS_CORE_ERROR_INVAL = static_cast<s32>(0x80410702u);
inline constexpr s32 CELL_SPURS_CORE_ERROR_STAT = static_cast<s32>(0x8041070Fu);
inline constexpr s32 CELL_SPURS_CORE_ERROR_ALIGN = static_cast<s32>(0x80410710u);
inline constexpr s32 CELL_SPURS_CORE_ERROR_NULL_POINTER = static_cast<s32>(0x80410711u);

class CellSpurs {
public:
    void Register(HleRegistry& registry);

    s32 Initialize(u32 spurs, u32 num_spus);
    s32 Finalize(u32 spurs);
    [[nodiscard]] bool IsInitialized(u32 spurs) const;

private:
    mutable std::mutex mutex_;
    std::set<u32> instances_;  // EAs of initialized CellSpurs objects
};

}  // namespace cell::hle
