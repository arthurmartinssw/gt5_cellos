// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/hle_registry.h
//
// High-level emulation of imported PS3 library functions (cellGcm*, cellFs*, cellAudio*,
// sys_* from sysPrxForUser, ...), keyed by (module name, FNID).
//
// How bloodborne_pc does it (probe.c + link_modules.py): imports are x86-64 relocations
// against symbols named "<NID>#<lib>#<module>". The runtime resolves each name to a *host*
// function pointer (runtime_memory_resolve, RUNTIME_LOOKUP(exports, name)) and writes it
// into the GOT; the game then calls host code directly with the SysV/Win64 ABI glue (ABI).
//
// Why that cannot work on PS3: the caller is PowerPC code. A PPU import call goes through a
// stub in .text that loads an OPD (function descriptor: {u32 code, u32 toc}) from a slot in
// .data, then branches to the code address with the callee's TOC in r2. So we give every
// import a guest-visible OPD whose code address points at a two-instruction guest thunk:
//
//     +0  0x04000000 | index    primary opcode 1: unused by the Cell PPU ISA, decoded by the
//                                JIT as "call HLE function <index>" (RPCS3 uses the same
//                                trick for its HACK instruction)
//     +4  blr                    return to the caller once the host handler is done
//     +8  OPD.code = thunk+0     what the import slot points to
//     +12 OPD.toc  = 0
//
// The handler reads arguments from r3..r10 / f1..f13 and writes the result to r3 (or f1),
// exactly like the PS4 runtime's ABI functions read rdi/rsi/... — only through the context.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "memory/guest_memory.h"
#include "ppu/ppu_context.h"

namespace cell::hle {

inline constexpr u32 kHleOpcode = 0x04000000u;  // primary opcode 1
inline constexpr u32 kHleIndexMask = 0x03FFFFFFu;
inline constexpr u32 kPpcBlr = 0x4E800020u;
inline constexpr u32 kThunkSize = 16;
inline constexpr u32 kThunksPerChunk = vm::k64K / kThunkSize;  // 4096 thunks per 64 KiB chunk
inline constexpr u32 kMaxFunctions = 1u << 16;

struct HleFunction;
using HleFunc = void (*)(ppu::PpuThreadContext& ctx, vm::GuestMemory& memory, const HleFunction& self);

struct HleFunction {
    HleFunction(std::string module_, u32 nid_, std::string name_, HleFunc fn_, bool implemented_, void* user_)
        : module(std::move(module_)), nid(nid_), name(std::move(name_)), fn(fn_), implemented(implemented_),
          user(user_) {}

    std::string module;  // e.g. "cellGcmSys"
    u32 nid;             // FNID from the libstub table
    std::string name;    // human-readable, when known
    HleFunc fn;
    bool implemented;
    void* user;     // owning service (e.g. the SysPrxForUser instance) for stateful handlers
    u32 thunk = 0;  // guest EA of the thunk (0 until materialized); OPD at thunk + 8
    mutable std::atomic<u64> calls{0};
};

[[nodiscard]] inline bool DecodeHleOpcode(u32 instruction, u32* index) noexcept {
    if ((instruction & ~kHleIndexMask) != kHleOpcode) return false;
    *index = instruction & kHleIndexMask;
    return true;
}

// The guest instruction that called an import, for diagnostics: "0x00ce7bd8: bl 0x00e1a2c0".
// `lr` is the guest EA the PPU interpreter holds in LR when the HLE thunk runs (guest
// addresses are used directly, no translation); PS3 import stubs end in `bctr`, so LR - 4 is
// the caller's `bl` to the stub. Empty if that word is not readable guest memory.
[[nodiscard]] std::string DescribeCallSite(const vm::GuestMemory& memory, u64 lr);

class HleRegistry {
public:
    explicit HleRegistry(vm::GuestMemory& memory);

    // Host implementation of an importable function.
    void Register(std::string_view module, u32 nid, std::string_view name, HleFunc fn, void* user = nullptr);
    // Same, with the FNID computed from the export name.
    void Register(std::string_view module, std::string_view name, HleFunc fn, void* user = nullptr);
    // Index for an import; unknown (module, nid) pairs get a logging stub returning CELL_OK.
    u32 Bind(std::string_view module, u32 nid);
    // Writes thunks + OPDs for every entry not yet placed in guest memory. Idempotent.
    void Materialize();

    [[nodiscard]] u32 CodeAddress(u32 index) const noexcept { return slots_[index]->thunk; }
    [[nodiscard]] u32 OpdAddress(u32 index) const noexcept { return slots_[index]->thunk + 8; }
    [[nodiscard]] const HleFunction& At(u32 index) const noexcept { return *slots_[index]; }
    // Published count: every index below it refers to a fully constructed entry.
    [[nodiscard]] u32 size() const noexcept { return count_.load(std::memory_order_acquire); }
    // Returning from the primary thread's entry lands here (LR of the first frame).
    [[nodiscard]] u32 exit_index() const noexcept { return exit_index_; }
    // Returning from any other PPU thread's entry: sys_ppu_thread_exit(r3).
    [[nodiscard]] u32 thread_exit_index() const noexcept { return thread_exit_index_; }

    // Called by the PPU JIT/interpreter when it meets kHleOpcode | index.
    void Dispatch(u32 index, ppu::PpuThreadContext& ctx);

private:
    u32 AddLocked(std::string module, u32 nid, std::string name, HleFunc fn, bool implemented, void* user);
    static std::string Key(std::string_view module, u32 nid);

    vm::GuestMemory& memory_;
    std::mutex mutex_;  // registration, binding, materialization (boot / PRX load)
    // Thread-safety model: a fixed array of slots, filled under mutex_ and published by
    // count_ (release). Dispatch from any PPU thread reads slots_[i] for i < count_ without a
    // lock; slots never move or get freed while the process runs. `calls` is atomic; the
    // other fields are written before publication, or (Register upgrading a stub) only at
    // boot, before any PPU thread runs.
    std::unique_ptr<std::unique_ptr<HleFunction>[]> slots_;
    std::atomic<u32> count_{0};
    std::unordered_map<std::string, u32> lookup_;
    std::vector<u32> chunks_;  // guest EAs of the 64 KiB thunk chunks
    u32 placed_ = 0;
    u32 exit_index_ = 0;
    u32 thread_exit_index_ = 0;
};

}  // namespace cell::hle
