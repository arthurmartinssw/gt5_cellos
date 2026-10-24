// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — spu/spu_run_stats.h
//
// Per-group run statistics for SPU thread groups, and the logging policy for their start /
// exit messages.
//
// Why: SPURS instances created with exitIfNoWork (the last argument of
// _cellSpursAttributeInitialize) release their SPUs whenever no workload is ready — the SPURS
// kernel calls sys_spu_thread_group_exit(0) — and the instance's handler PPU thread
// (<prefix>SpursHdlr0) waits until a workload becomes ready again, then starts and joins the
// group once more. A middleware that kicks SPU work at a fixed rate therefore produces one
// start/exit pair per kick: at the cellAudio block rate (187.5/s) that is thousands of
// identical log lines per minute. Each line is legitimate, so nothing is suppressed in the
// runtime; only the log is bounded:
//   * the first `first_runs` starts/exits of every group are logged as before;
//   * afterwards each run is counted, and a summary per group is logged every
//     `summary_seconds` (runs, rate, SPU instructions per run, run length, start-to-start
//     interval range, exit causes), which is what tells a paced dispatch cycle (steady rate,
//     real work per run) from a restart loop (very short runs as fast as the host allows);
//   * `verbose` (--log-spu-runs) restores every message.
// Faults, terminations and warnings are never affected.
#pragma once

#include <atomic>
#include <map>
#include <string>
#include <utility>

#include "common/types.h"

namespace cell::spu {

struct SpuRunLogConfig {
    std::atomic<bool> verbose{false};
    std::atomic<u32> first_runs{3};
    std::atomic<u32> summary_seconds{10};  // 0: no periodic summary
};
// Process-wide policy (like log::Threshold()).
SpuRunLogConfig& RunLogConfig() noexcept;

[[nodiscard]] const char* ExitCauseName(s32 join_cause) noexcept;

// Guarded by the owning group's mutex.
struct SpuRunStats {
    u64 starts = 0;
    u64 runs = 0;  // completed (all threads stopped)
    u64 total_insns = 0;
    u64 total_run_us = 0;
    u64 last_start_us = 0;
    u64 last_insns = 0;
    u64 last_run_us = 0;
    s32 last_cause = 0;
    s32 last_status = 0;
    std::map<std::pair<s32, s32>, u64> exits;  // (join cause, exit status) -> runs

    // Summary window (reset by TakeWindowSummary).
    u64 window_start_us = 0;
    u64 window_runs = 0;
    u64 window_insns = 0;
    u64 window_run_us = 0;
    u64 window_min_gap_us = ~u64{0};
    u64 window_max_gap_us = 0;
    std::map<std::pair<s32, s32>, u64> window_exits;

    void OnStart(u64 now_us);
    void OnStop(u64 now_us, u64 insns, s32 cause, s32 status);
    [[nodiscard]] bool WindowDue(u64 now_us, u32 seconds) const noexcept;
    // One summary line for the window ending at `now_us`, then starts a new window.
    std::string TakeWindowSummary(u64 now_us, u32 group_id, const std::string& name);
    // All-time totals, for the end / stall reports.
    [[nodiscard]] std::string Totals() const;
};

}  // namespace cell::spu
