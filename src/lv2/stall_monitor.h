// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/stall_monitor.h
//
// Deadlock / stall diagnostics. A host thread samples a progress counter every 250 ms:
// the sum, over every PPU thread, of instructions executed and calls (HLE + syscalls) made.
// When it has not moved for `stall_seconds`, every PPU thread is either blocked inside a
// call or gone. A second trigger watches the primary thread alone: background threads paced by
// a clock (audio, SPURS handlers) keep the global counter moving while a main loop is stuck.
// Either way the monitor prints, once per episode:
//   * each PPU thread: running/blocked, the syscall or HLE function it is inside (with its
//     first arguments and the caller's LR), last sampled PC, instruction count;
//   * each SPU thread group and thread: state, sampled PC, the channel or stop-and-signal
//     service it is inside (SPU_RdInMbox, receive_event...), mailbox and event state;
//   * every lv2 sync object with waiters or an owner (event queues, mutexes/lwmutexes,
//     conds, semaphores, event flags);
//   * extra sections from the front end (RSX FIFO put/get).
// The monitor keeps running while the process shuts down (Stop() is called after
// Kernel::Shutdown()), so a thread that never honours termination is reported too.
// `periodic_seconds` (optional) prints the same report every N seconds regardless, for hangs
// where a thread spins (polling loops keep the counter moving).
//
// Reading is lock-free for thread status (relaxed atomics updated by the interpreters) and
// try_lock for objects, so the report itself can never deadlock the process.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lv2/kernel.h"

namespace cell::lv2 {

class StallMonitor {
public:
    using Section = std::function<std::vector<std::string>()>;

    StallMonitor(Kernel& kernel, u32 stall_seconds, u32 periodic_seconds = 0);
    ~StallMonitor();
    StallMonitor(const StallMonitor&) = delete;
    StallMonitor& operator=(const StallMonitor&) = delete;

    // Extra report section (title + lines). Add before Start().
    void AddSection(std::string title, Section section);
    void Start();
    void Stop();  // idempotent; joins the monitor thread

    // Builds the report now (also used by tests). `reason` goes in the header line.
    [[nodiscard]] std::string BuildReport(const std::string& reason) const;
    // Current progress counter (PPU instructions + calls).
    [[nodiscard]] u64 Progress() const;
    [[nodiscard]] u32 reports_printed() const noexcept { return reports_.load(); }

private:
    void Main();
    // Instructions + calls of the running primary thread (nullopt once it is gone).
    [[nodiscard]] std::optional<u64> PrimaryProgress() const;

    Kernel& kernel_;
    const u32 stall_seconds_;
    const u32 periodic_seconds_;
    std::vector<std::pair<std::string, Section>> sections_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::atomic<u32> reports_{0};
};

}  // namespace cell::lv2
