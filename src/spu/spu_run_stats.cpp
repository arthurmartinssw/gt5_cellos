// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — spu/spu_run_stats.cpp
#include "spu/spu_run_stats.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

namespace cell::spu {

SpuRunLogConfig& RunLogConfig() noexcept {
    static SpuRunLogConfig config;
    return config;
}

const char* ExitCauseName(s32 cause) noexcept {
    switch (cause) {
    case 0x0001: return "GROUP_EXIT";
    case 0x0002: return "ALL_THREADS_EXIT";
    case 0x0004: return "TERMINATED";
    default: return "?";
    }
}

void SpuRunStats::OnStart(u64 now_us) {
    if (starts) {
        const u64 gap = now_us > last_start_us ? now_us - last_start_us : 0;
        window_min_gap_us = std::min(window_min_gap_us, gap);
        window_max_gap_us = std::max(window_max_gap_us, gap);
    }
    if (!window_start_us) window_start_us = now_us;
    ++starts;
    last_start_us = now_us;
}

void SpuRunStats::OnStop(u64 now_us, u64 insns, s32 cause, s32 status) {
    const u64 run_us = now_us > last_start_us ? now_us - last_start_us : 0;
    ++runs;
    total_insns += insns;
    total_run_us += run_us;
    last_insns = insns;
    last_run_us = run_us;
    last_cause = cause;
    last_status = status;
    ++exits[{cause, status}];
    ++window_runs;
    window_insns += insns;
    window_run_us += run_us;
    ++window_exits[{cause, status}];
}

bool SpuRunStats::WindowDue(u64 now_us, u32 seconds) const noexcept {
    return seconds && window_runs && now_us >= window_start_us + u64{seconds} * 1'000'000;
}

namespace {
std::string ExitList(const std::map<std::pair<s32, s32>, u64>& exits) {
    std::string out;
    char item[64];
    for (const auto& [key, count] : exits) {
        std::snprintf(item, sizeof(item), "%s%s(%d) x%" PRIu64, out.empty() ? "" : ", ", ExitCauseName(key.first), key.second, count);
        out += item;
    }
    return out.empty() ? "none" : out;
}
}  // namespace

std::string SpuRunStats::TakeWindowSummary(u64 now_us, u32 group_id, const std::string& name) {
    const double seconds = std::max<double>(1e-6, static_cast<double>(now_us - window_start_us) / 1e6);
    const double rate = static_cast<double>(window_runs) / seconds;
    const u64 avg_insns = window_runs ? window_insns / window_runs : 0;
    const double avg_run_ms = window_runs ? static_cast<double>(window_run_us) / static_cast<double>(window_runs) / 1000.0 : 0.0;
    char gaps[96] = "n/a";
    if (window_min_gap_us != ~u64{0})
        std::snprintf(gaps, sizeof(gaps), "%.2f..%.2f ms", static_cast<double>(window_min_gap_us) / 1000.0,
                      static_cast<double>(window_max_gap_us) / 1000.0);
    char line[512];
    std::snprintf(line, sizeof(line),
                  "SPU group 0x%08x \"%s\": %" PRIu64 " run(s) in %.1f s (%.1f/s), avg %" PRIu64
                  " SPU insns/run, avg run %.3f ms, start interval %s, exits: %s; total %" PRIu64 " run(s)%s",
                  group_id, name.c_str(), window_runs, seconds, rate, avg_insns, avg_run_ms, gaps, ExitList(window_exits).c_str(),
                  runs,
                  // Heuristic only: a dispatch cycle does real work per run; an idle restart loop
                  // boots the kernel, finds nothing and exits, as fast as the host allows.
                  rate > 1000.0 && avg_insns < 2000 ? " [heuristic: very short runs at a high rate — check for a restart loop]" : "");
    window_start_us = now_us;
    window_runs = window_insns = window_run_us = 0;
    window_min_gap_us = ~u64{0};
    window_max_gap_us = 0;
    window_exits.clear();
    return line;
}

std::string SpuRunStats::Totals() const {
    char line[384];
    std::snprintf(line, sizeof(line), "%" PRIu64 " start(s), %" PRIu64 " completed run(s), avg %" PRIu64
                  " SPU insns/run, last run %" PRIu64 " insns / %.3f ms, exits: %s",
                  starts, runs, runs ? total_insns / runs : 0, last_insns, static_cast<double>(last_run_us) / 1000.0,
                  ExitList(exits).c_str());
    return line;
}

}  // namespace cell::spu
