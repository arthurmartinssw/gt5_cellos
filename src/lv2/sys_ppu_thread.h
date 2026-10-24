// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_ppu_thread.h
//
// PPU threads. Syscalls (lv2 numbering):
//   41 _sys_ppu_thread_exit        43 sys_ppu_thread_yield        44 sys_ppu_thread_join
//   45 sys_ppu_thread_detach       46 sys_ppu_thread_get_join_state
//   47 sys_ppu_thread_set_priority 48 sys_ppu_thread_get_priority
//   49 sys_ppu_thread_get_stack_information
//   52 _sys_ppu_thread_create      53 sys_ppu_thread_start        56 sys_ppu_thread_rename
// Games call sysPrxForUser's sys_ppu_thread_create (HLE), which allocates the TLS block and
// does 52 + 53 through ThreadManager directly.
//
// Lifecycle:  Create -> (Start) host std::thread runs PpuInterpreter::Execute ->
//   * thread exit (entry returned / sys_ppu_thread_exit): state Exited, joiners woken;
//     a detached thread is reaped at once, a joinable one when joined.
//   * process exit / exitspawn / fatal stop: Kernel::RequestTermination(), all threads stop.
// Host std::thread objects are kept until Shutdown() joins them all.
#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "lv2/kernel.h"

namespace cell::lv2 {

inline constexpr u64 SYS_PPU_THREAD_CREATE_JOINABLE = 0x1;
inline constexpr u64 SYS_PPU_THREAD_CREATE_INTERRUPT = 0x2;

struct PpuThread {
    enum class State { Created, Running, Exited };

    u32 id = 0;
    std::string name;
    std::string log_name;  // "0x01000001 name", stable storage for log::tls_thread_name
    ppu::PpuThreadContext ctx{};
    s32 priority = 0;
    u32 stack_base = 0;    // allocation base (guard included)
    u32 stack_size = 0;    // allocation size
    u32 tls_block = 0;     // 0 if the creator owns it
    bool joinable = false;
    bool joining = false;  // someone is already in sys_ppu_thread_join on it
    bool primary = false;
    State state = State::Created;
    u64 exit_value = 0;
    ppu::StopInfo stop;
    std::vector<u32> recent_pcs;
    std::vector<ppu::CallRecord> recent_calls;
    ppu::PpuLiveStatus live;  // updated by the interpreter, read lock-free by the stall report
};

class ThreadManager {
public:
    struct CreateParams {
        u32 entry_opd = 0;   // OPD {code, toc}
        u32 tls_pointer = 0; // initial r13 (0: none)
        u32 tls_block = 0;   // freed with the thread when non-zero
        u64 arg = 0;         // r3
        u64 arg2 = 0;        // r4 (module_start's argp; sys_ppu_thread_create passes only r3)
        u64 arg3 = 0;        // r5 (guest callbacks with three arguments: sysutil status callbacks)
        s32 priority = 1001;
        u32 stack_size = 0x10000;
        u64 flags = 0;
        std::string name;
    };
    struct ThreadInfo {
        u32 id;
        std::string name;
        PpuThread::State state;
        u32 pc;
        u64 exit_value;
        bool primary;
        ppu::StopReason stop;
        // Live view (running threads): where it is and what call it is inside, if any.
        u32 live_pc = 0;
        u8 live_kind = ppu::PpuLiveStatus::kRunning;
        u32 live_call = 0;
        u32 live_lr = 0;
        u64 live_args[4] = {};
        u64 live_instructions = 0;
        u64 live_calls = 0;
        // Last completed calls, oldest first (CallRecord.pc = the caller's LR, args[2..3] = 0).
        std::vector<ppu::CallRecord> live_recent{};
    };

    explicit ThreadManager(Kernel& kernel) noexcept : kernel_(kernel) {}
    ~ThreadManager();

    // ---- primary thread (runs on the calling host thread) ----
    void AdoptPrimary(const ppu::PpuThreadContext& ctx, s32 priority, std::string name);
    // Runs the primary thread until it stops. Returns its stop info.
    ppu::StopInfo RunPrimary(u64 max_instructions, bool trace);
    [[nodiscard]] const ppu::PpuThreadContext& primary_context() const;
    // After the primary thread exited as a *thread*: wait until every other thread is gone
    // or the process is terminated.
    void WaitForAllThreads();
    // Terminate (if not yet) and join every host thread. Idempotent.
    void Shutdown();

    // ---- syscall-level operations; return CELL_* ----
    s32 Create(const CreateParams& params, u32* out_id);
    s32 Start(u32 id);
    s32 Join(const ppu::PpuThreadContext& caller, u32 id, u64* exit_value);
    s32 Detach(const ppu::PpuThreadContext& caller, u32 id);
    s32 GetJoinState(const ppu::PpuThreadContext& caller, bool* joinable);
    s32 SetPriority(u32 id, s32 priority);
    s32 GetPriority(u32 id, s32* priority);
    s32 Rename(u32 id, std::string name);

    // Calls guest function `opd` (r3 = a0, r4 = a1) on a new joinable PPU thread and waits for
    // it on the calling host thread; returns its r3. Used for module_start / module_stop: the
    // caller may itself be a PPU thread blocked inside an HLE call (sys_prx_start_module),
    // which is fine because every PPU thread is a host thread. nullopt if it could not run or
    // the process ended meanwhile.
    std::optional<u64> CallGuest(u32 opd, u64 a0, u64 a1, std::string name, u32 stack_size = 0x10000);
    // Same with a third argument in r5.
    std::optional<u64> CallGuest3(u32 opd, u64 a0, u64 a1, u64 a2, std::string name, u32 stack_size = 0x10000);

    // TLS block for a new thread from the process PT_TLS image: returns {block, r13}.
    std::optional<std::pair<u32, u32>> AllocateTls();

    void WakeForTermination();
    [[nodiscard]] std::vector<ThreadInfo> Snapshot() const;
    void RegisterSyscalls(SyscallTable& table);

private:
    void HostMain(std::shared_ptr<PpuThread> thread);
    void OnStopped(PpuThread& thread, const ppu::StopInfo& stop, const ppu::PpuInterpreter& cpu);
    void ReapLocked(PpuThread& thread);
    std::shared_ptr<PpuThread> FindLocked(u32 id) const;

    Kernel& kernel_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;  // state changes (exit, termination)
    std::unordered_map<u32, std::shared_ptr<PpuThread>> threads_;
    std::vector<std::thread> host_threads_;
    std::shared_ptr<PpuThread> primary_;
    u32 next_id_ = kIdPpuThread + 1;
    bool shut_down_ = false;
};

}  // namespace cell::lv2
