// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — ppu/ppu_interpreter.h
//
// Reference PPU interpreter. Purpose: execute guest code *correctly* and report precisely
// where and why it stops, so the JIT (phase 2b) can be validated instruction by instruction
// against it.
//
// Design:
//   * Predecoded pages, shared by all PPU threads (PpuCodeCache). The first time any thread
//     enters a 4 KiB page, all 1024 words are byte-swapped and decoded once and published
//     with their handler pointers. Steady-state cost per instruction: one acquire load, one
//     indirect call. A guest store into a code page, icbi, or a free drops the page.
//   * One PpuInterpreter per PPU thread (per host thread): it holds only per-thread state
//     (next PC, reservation, stop info, PC history). Nothing in it is shared.
//   * Cross-thread memory: aligned guest loads/stores are relaxed std::atomic_ref accesses
//     (plain MOVs on x86, but defined behaviour under the C++ memory model and clean under
//     ThreadSanitizer). sync -> seq_cst fence (orders store->load, which x86 can reorder),
//     lwsync/eieio -> acq_rel fence. PPC is weaker than x86-TSO, so this is sufficient.
//   * `kill` (optional): an atomic flag the kernel raises when the process terminates; checked
//     once per instruction, it ends Execute() with StopReason::Terminated.
//   * Explicit memory checks. Every load/store tests the lock-free page flags (1 byte per
//     4 KiB) and turns a bad access into a precise StopInfo (PC, EA, read/write) instead of
//     a host crash. The JIT will instead rely on host faults + GuestAddressOf().
//   * Big-endian everywhere at the boundary: guest memory is big-endian, registers are
//     host values. Integer/FP accesses ByteSwap; 16-byte VMX accesses copy bytes verbatim
//     because v128 keeps big-endian element order (so vperm/lvsl indices work unchanged).
//   * HLE: primary opcode 1 calls HleRegistry::Dispatch; `sc` calls SyscallTable::Dispatch.
//     Either may set ctx.exit_requested, which ends Execute() after that instruction.
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include <atomic>

#include "hle/hle_registry.h"
#include "lv2/syscall_table.h"
#include "memory/guest_memory.h"
#include "ppu/ppu_code_cache.h"
#include "ppu/ppu_context.h"
#include "ppu/ppu_instructions.h"
#include "ppu/ppu_opcode.h"

namespace cell::ppu {

enum class StopReason : u8 {
    None,
    Exited,         // ctx.exit_requested (see ctx.exit_kind / ctx.exit_status)
    Unimplemented,  // decoder or handler does not know the instruction
    InstrFault,     // PC not in readable memory / misaligned
    DataFault,      // load/store to unmapped or protected memory
    Alignment,      // misaligned lwarx/stwcx. family
    Trap,           // tw/td/twi/tdi condition met (guest assert / breakpoint)
    Privileged,     // supervisor-only instruction or SPR
    StepLimit,      // instruction budget exhausted
    Terminated,     // the process is going down (another thread exited the process or crashed)
};

[[nodiscard]] const char* StopReasonName(StopReason reason) noexcept;

// One HLE call or syscall made by a thread, for crash reports ("which call returned the
// value that later crashed?").
struct CallRecord {
    u32 pc = 0;          // address of the sc / of the HLE thunk
    bool syscall = false;
    u32 id = 0;          // syscall number or HLE index
    u64 args[4] = {};    // r3..r6 on entry
    u64 result = 0;      // r3 on return
};

// What a running thread is doing, readable from any host thread without locks (the stall
// report reads it while the thread is blocked inside an HLE call or a syscall). Written with
// relaxed stores: a slightly stale view is fine for diagnostics.
struct PpuLiveStatus {
    enum Kind : u8 { kRunning = 0, kInHle = 1, kInSyscall = 2, kInPageFault = 3 };  // page fault: args[0] = address
    std::atomic<u32> pc{0};           // last sampled PC (every kSampleInterval instructions and at calls)
    std::atomic<u8> kind{kRunning};   // inside a call right now?
    std::atomic<u32> call_id{0};      // syscall number or HLE index
    std::atomic<u32> lr{0};           // LR at call entry: the guest code that made the call
    std::atomic<u64> args[4]{};       // r3..r6 at call entry
    std::atomic<u64> instructions{0}; // executed so far (sampled)
    std::atomic<u64> calls{0};        // HLE calls + syscalls made so far
    static constexpr u64 kSampleInterval = 4096;

    // The last completed calls (ring), so the stall report can show what a blocked thread did
    // just before the call it is stuck in. Each field is its own relaxed atomic: a reader racing
    // with the writer can see a mixed entry — acceptable for a diagnostic, never for logic.
    struct RecentCall {
        std::atomic<u32> id{0};        // syscall number or HLE index
        std::atomic<u8> syscall{0};
        std::atomic<u32> lr{0};
        std::atomic<u64> arg0{0}, arg1{0};
        std::atomic<u64> result{0};
    };
    static constexpr u32 kRecentCalls = 8;
    std::array<RecentCall, kRecentCalls> recent{};
    std::atomic<u64> completed{0};    // calls returned so far (index of the next ring slot)
    void Complete(u32 id, bool is_syscall, u32 call_lr, u64 a0, u64 a1, u64 res) noexcept {
        const u64 n = completed.load(std::memory_order_relaxed);
        RecentCall& r = recent[n % kRecentCalls];
        r.id.store(id, std::memory_order_relaxed);
        r.syscall.store(is_syscall ? 1 : 0, std::memory_order_relaxed);
        r.lr.store(call_lr, std::memory_order_relaxed);
        r.arg0.store(a0, std::memory_order_relaxed);
        r.arg1.store(a1, std::memory_order_relaxed);
        r.result.store(res, std::memory_order_relaxed);
        completed.store(n + 1, std::memory_order_release);
    }
};

struct StopInfo {
    StopReason reason = StopReason::None;
    u32 pc = 0;           // instruction that stopped (or next PC for Exited/StepLimit)
    u32 instruction = 0;  // its encoding
    u32 address = 0;      // faulting EA (DataFault/Alignment)
    bool write = false;
    u64 executed = 0;     // instructions completed by this Execute() call
    std::string detail;
};

// Called on the faulting thread when a load/store hits a page that is not accessible.
// Returns true when the access should be retried (the guest mapped the page and recovered
// the thread), false for a real fault. May block (demand paging, lv2/sys_mmapper.h).
using PageFaultHandler = bool (*)(void* user, PpuThreadContext& ctx, u32 ea, bool write);

class PpuInterpreter final : public IPpuExecutor {
public:
    static constexpr u64 kTimebaseFrequency = 79'800'000;  // Cell timebase: 79.8 MHz

    PpuInterpreter(vm::GuestMemory& memory, hle::HleRegistry& hle, lv2::SyscallTable& syscalls,
                   PpuCodeCache& code, const std::atomic<bool>* kill = nullptr);
    ~PpuInterpreter() override;
    PpuInterpreter(const PpuInterpreter&) = delete;
    PpuInterpreter& operator=(const PpuInterpreter&) = delete;

    void Run(PpuThreadContext& ctx) override;  // until any stop
    StopInfo Execute(PpuThreadContext& ctx, u64 max_instructions);

    void set_trace(bool on) noexcept { trace_ = on; }
    // Optional live status (owned by the caller, must outlive Execute()).
    void set_live_status(PpuLiveStatus* live) noexcept { live_ = live; }
    void set_page_fault_handler(PageFaultHandler handler, void* user) noexcept {
        pf_handler_ = handler;
        pf_user_ = user;
    }
    void InvalidateCode(u32 addr, u64 size) noexcept;
    [[nodiscard]] const StopInfo& last_stop() const noexcept { return stop_; }
    // Most recent PCs, oldest first (crash reports).
    [[nodiscard]] std::vector<u32> RecentPcs() const;
    // Most recent HLE calls and syscalls, oldest first.
    [[nodiscard]] std::vector<CallRecord> RecentCalls() const;

    [[nodiscard]] static u64 TimeBase() noexcept;
    // Guest system time (sys_time_get_system_time), microseconds on the same epoch as the
    // timebase, so cellAudio timestamps, mftb and the system time agree.
    [[nodiscard]] static u64 SystemTimeMicroseconds() noexcept { return TimeBase() * 10 / 798; }

private:
    friend struct Ops;

    void Stop(PpuThreadContext& ctx, StopReason reason, u32 address = 0, bool write = false, std::string detail = {});

    // ---- guest memory access (big-endian, checked) ----
    [[nodiscard]] bool Accessible(u32 ea, u32 size, u8 need) const noexcept;
    // Slow path of every access check: offers the fault to the page-fault handler and retries
    // until the access is possible (true) or the fault is real (false, caller stops).
    bool RecoverFault(PpuThreadContext& ctx, u32 ea, u32 size, u8 need, bool write);
    template <typename T>
    bool Read(PpuThreadContext& ctx, u32 ea, T& out) noexcept;
    template <typename T>
    bool Write(PpuThreadContext& ctx, u32 ea, T value) noexcept;
    bool ReadBytes(PpuThreadContext& ctx, u32 ea, void* out, u32 size) noexcept;
    bool WriteBytes(PpuThreadContext& ctx, u32 ea, const void* in, u32 size) noexcept;
    void NoteCodeWrite(u32 ea, u32 size) noexcept;
    void EnterCall(const PpuThreadContext& ctx, PpuLiveStatus::Kind kind, u32 id) noexcept {
        if (!live_) return;
        live_->pc.store(ctx.cia, std::memory_order_relaxed);
        live_->call_id.store(id, std::memory_order_relaxed);
        live_->lr.store(static_cast<u32>(ctx.lr), std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) live_->args[i].store(ctx.gpr[3 + i], std::memory_order_relaxed);
        live_->calls.fetch_add(1, std::memory_order_relaxed);
        live_->kind.store(kind, std::memory_order_release);
    }
    void LeaveCall(const CallRecord& rec, u32 call_lr) noexcept {
        if (!live_) return;
        live_->Complete(rec.id, rec.syscall, call_lr, rec.args[0], rec.args[1], rec.result);
        live_->kind.store(PpuLiveStatus::kRunning, std::memory_order_release);
    }

    vm::GuestMemory& memory_;
    hle::HleRegistry& hle_;
    lv2::SyscallTable& syscalls_;
    PpuCodeCache& code_;
    const std::atomic<bool>* kill_;
    u8* base_;

    // Per-instruction state.
    u32 nia_ = 0;          // next instruction address; branches overwrite it
    bool stopped_ = false;
    StopInfo stop_;
    u32 current_raw_ = 0;

    // lwarx/ldarx reservation (single PPU thread model for now: value-based CAS).
    bool reserved_ = false;
    u32 reserve_addr_ = 0;
    u64 reserve_raw_ = 0;  // big-endian bytes as they sit in memory
    u32 reserve_size_ = 0;

    bool trace_ = false;
    std::array<u32, 64> history_{};
    u64 history_count_ = 0;
    std::array<CallRecord, 16> calls_{};
    u64 call_count_ = 0;
    PpuLiveStatus* live_ = nullptr;
    PageFaultHandler pf_handler_ = nullptr;
    void* pf_user_ = nullptr;
    u64 total_executed_ = 0;
};

}  // namespace cell::ppu
