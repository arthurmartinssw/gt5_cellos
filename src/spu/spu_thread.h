// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — spu/spu_thread.h
//
// SPU threads: the host side of everything an SPU program touches except its instructions.
//
// How PS3 concepts map to the PC (no hardware simulation, direct host mapping):
//   SPU core            -> one host std::thread per running SPU thread (named "SPU 0x...")
//   256 KiB local store -> 256 KiB of the guest arena: a 1 MiB window in the spu_thread area
//                          (0xF0000000+), LS at its start, the rest unmapped (guard). The
//                          executor/JIT gets a plain host pointer (memory.base() + EA) and
//                          masks LS addresses with 0x3FFFF.
//   MFC DMA             -> synchronous memcpy between that LS and the arena (spu_mfc.cpp);
//                          tags complete immediately, so tag-status waits never block.
//   Channels            -> host data + one mutex/condition_variable per SPU thread; blocking
//                          channel reads/writes wait on it (and on process termination).
//   lv2 SPU services    -> stop codes and the out-interrupt-mailbox event protocol are handled
//                          here in C++ (send/throw event, event flag bits, receive event).
//
// The instruction side is ISpuExecutor: the SPU interpreter / SPU->x86-64 JIT (next phase)
// implements Run() and calls ReadChannel / WriteChannel / ChannelCount / StopAndSignal for
// rdch, wrch, rchcnt and stop. Until one is installed, starting an SPU thread group ends the
// process with an explicit report instead of pretending the SPU work was done.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "lv2/kernel.h"
#include "ppu/ppu_context.h"
#include "spu/spu_defs.h"
#include "spu/spu_elf.h"
#include "spu/spu_run_stats.h"

namespace cell::lv2 {
struct Lv2EventQueue;
}

namespace cell::spu {

struct SpuThread;
struct SpuThreadGroup;

// Architectural state the executor/JIT works on. Registers are 128-bit, big-endian element
// order (byte 0 = most significant), like the PPU's v128: the "preferred slot" is bytes 0..3.
struct alignas(64) SpuContext {
    ppu::v128 gpr[128];
    u32 pc = 0;    // LS address of the next instruction
    u32 srr0 = 0;  // interrupt return address
    u8* ls = nullptr;  // host pointer to the 256 KiB local store
    u32 ls_ea = 0;     // its guest EA (inside the spu_thread area)
};

class ISpuExecutor {
public:
    virtual ~ISpuExecutor() = default;
    // Runs `thread` until StopAndSignal / a channel call returns false or thread.ShouldStop().
    virtual void Run(SpuThread& thread) = 0;
    // Instructions executed by the last Run() (diagnostics; 0 if the executor does not count).
    [[nodiscard]] virtual u64 executed() const noexcept { return 0; }
};

enum class SpuExit : u8 { None, ThreadExit, GroupExit, Terminated, StopCode, Fault };

// Per-thread MFC state (command parameters written through channels 16..21).
struct MfcState {
    u32 lsa = 0, eah = 0, eal = 0, size = 0, tag = 0;
    u32 tag_mask = 0;
    u32 atomic_stat = 0;
    bool atomic_pending = false;
    bool reservation_valid = false;
    u32 reservation_ea = 0;
    std::array<u8, kReservationSize> reservation_data{};
    u32 stall_mask = 0;  // tags whose list hit a stall-and-notify element (MFC_RdListStallStat)
    bool stall_notify_warned = false;
};

struct SpuThread : lv2::Lv2Object {
    SpuThread(lv2::Kernel& kernel_, SpuThreadGroup& group_, u32 spu_num_) noexcept
        : kernel(kernel_), group(group_), spu_num(spu_num_) {}

    lv2::Kernel& kernel;
    SpuThreadGroup& group;  // the group owns its threads and outlives them
    const u32 spu_num;      // index in the group (EA window for group-relative DMA)
    std::string log_name;
    SpuContext ctx{};
    SpuElfInfo image;
    u64 args[4] = {};
    u32 option = 0;

    // ---- everything below is guarded by m ----
    std::mutex m;
    std::condition_variable cv;
    std::deque<u32> in_mbox;           // PPU -> SPU, 4 deep
    std::optional<u32> out_mbox;       // SPU -> PPU, 1 deep
    u32 snr[2] = {};
    bool snr_valid[2] = {};
    bool snr_or_mode[2] = {};
    u32 event_mask = 0;
    u32 event_stat = 0;
    u32 dec_value = 0;
    u64 dec_start_tb = 0;
    bool dec_event_fired = true;  // SPU_EVENT_TM is raised once per SPU_WrDec
    MfcState mfc;  // owned by the executor thread only (no lock needed)
    std::array<std::weak_ptr<lv2::Lv2EventQueue>, kSpuPortCount> ports;  // connect_event (spup)
    std::vector<std::pair<u32, std::weak_ptr<lv2::Lv2EventQueue>>> bound_queues;  // bind_queue (spuq)
    SpuExit exit = SpuExit::None;
    u32 exit_status = 0;
    std::string fault;
    std::atomic<bool> stop_requested{false};  // group terminate / group exit by a peer

    // ---- live status for the stall report (relaxed atomics, any thread may read) ----
    // live_wait: -1 running; 0..127 inside rdch/wrch on that channel (blocked if it stays);
    // kWaitStop | code inside a stop-and-signal service (0x110 = receive_event...).
    static constexpr s32 kWaitStop = 0x10000;
    static constexpr s32 kWaitPageFault = 0x20000;  // MFC transfer suspended on a guest page fault
    std::atomic<u32> live_pc{0};
    std::atomic<u64> live_instructions{0};
    std::atomic<s32> live_wait{-1};
    u64 last_run_insns = 0;  // set by HostMain before OnThreadStopped (read under the group lock)

    // ---- executor interface (called on this thread's host thread) ----
    [[nodiscard]] bool ShouldStop() const noexcept;
    bool ReadChannel(u32 channel, u32& value);
    bool WriteChannel(u32 channel, u32 value);
    [[nodiscard]] u32 ChannelCount(u32 channel);
    bool StopAndSignal(u32 code);
    // Ends the thread with a fault (bad DMA, invalid channel, executor error): the whole
    // process is terminated with `message` in the report.
    void Fault(std::string message);

    // ---- PPU-side operations (lv2 syscalls) ----
    void WriteInboundMailbox(u32 value);
    s32 WriteSignal(u32 number, u32 value);
    // Registers, entry PC, arguments and a fresh copy of the image segments for a (re)start.
    // Called with the group not running. False (with `error`) on an unloadable image.
    bool ResetForStart(std::string* error);
    // Wakes this thread wherever it blocks (channel wait or bound SPU event queue) so it can
    // observe stop_requested.
    void WakeWaiters();

    void WakeForTermination() override;
    void HostMain();  // body of the host thread

private:
    bool HandleOutIntrMailbox(std::unique_lock<std::mutex>& lock, u32 value);
    bool ReceiveEvent(std::unique_lock<std::mutex>& lock, bool try_only);
    bool FailLocked(std::unique_lock<std::mutex>& lock, std::string message);
    template <typename Pred>
    bool BlockUntil(std::unique_lock<std::mutex>& lock, Pred ready);
    u32 PendingEventsLocked();
    u32 DecrementerLocked() const;
};

inline constexpr s32 SYS_SPU_THREAD_GROUP_JOIN_GROUP_EXIT = 0x0001;
inline constexpr s32 SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT = 0x0002;
inline constexpr s32 SYS_SPU_THREAD_GROUP_JOIN_TERMINATED = 0x0004;

struct SpuThreadGroup : lv2::Lv2Object {
    enum class State { NotInitialized, Initialized, Running, Stopped };

    lv2::Kernel* kernel = nullptr;
    std::mutex m;
    // ---- guarded by m ----
    std::condition_variable cv;
    u32 num = 0;
    s32 priority = 0;
    s32 type = 0;
    std::vector<std::shared_ptr<SpuThread>> threads;  // size num, null until initialized
    State state = State::NotInitialized;
    u32 running = 0;
    s32 join_cause = 0;
    s32 exit_status = 0;
    bool joining = false;
    std::vector<std::thread> hosts;  // host threads of the current/last run
    // sys_spu_thread_group_connect_event: RUN (1), EXCEPTION (2), SYSTEM_MODULE (4)
    std::array<std::weak_ptr<lv2::Lv2EventQueue>, 3> event_queues;
    // Run statistics (guarded by m) and whether this run's start/exit is logged (lock-free read).
    SpuRunStats stats;
    u64 run_insns = 0;  // instructions of the current run, summed as threads stop (guarded by m)
    std::atomic<bool> log_this_run{true};

    // Peer lookup for group-relative DMA (EA 0xF0000000 + spu_num * 1 MiB + LS offset).
    [[nodiscard]] std::shared_ptr<SpuThread> Peer(u32 spu_num);
    void OnThreadStopped(SpuThread& thread);
    void RequestStop(s32 cause, s32 status);  // terminate / group exit: stop every thread
    void JoinHosts();                         // joins finished host threads (not under m)
    void WakeForTermination() override;
};

}  // namespace cell::spu
