// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — ppu/ppu_context.h
//
// Architectural state of one PPU hardware thread, laid out for the JIT: generated code keeps
// a pointer to this struct in a callee-saved host register and addresses fields with fixed
// displacements, so hot fields come first and the struct is cache-line aligned.
//
// PS4 -> PS3 note: probe.c never needed this. It jumped into the game with
// enter_on_stack(entry, args, exit, stack) and the host CPU *was* the guest CPU. On the PS3
// the guest CPU is a 64-bit big-endian PowerPC with VMX, so its registers live in memory and
// the PPU JIT (next phase) reads and writes them here.
#pragma once

#include <cstddef>

#include "common/types.h"

namespace cell::ppu {

struct alignas(16) v128 {
    u8 bytes[16];  // big-endian element order, as the VMX unit sees it
};

struct alignas(64) PpuThreadContext {
    u64 gpr[32];  // r0-r31
    u64 lr;
    u64 ctr;
    u64 xer;
    u32 cr;       // 8 x 4-bit fields
    u32 cia;      // current instruction address (EA)
    f64 fpr[32];
    u32 fpscr;
    u32 vscr;
    u32 vrsave;
    u32 thread_id;
    v128 vr[32];

    // Thread bookkeeping (not architectural).
    u32 stack_addr;
    u32 stack_size;
    u64 exit_status;
    bool exit_requested;  // set by HLE/syscalls; the executor stops after the current instruction
    u8 exit_kind;         // ExitKind
    const char* exit_message;  // kExitAbort: static string explaining why (never freed)
};

enum ExitKind : u8 {
    kExitProcess = 0,    // sys_process_exit / return from the entry function
    kExitThread = 1,     // sys_ppu_thread_exit
    kExitSpawn = 2,      // sys_game_process_exitspawn*: the process wants to load another SELF
    kExitAbort = 3,      // an HLE function the game needs cannot work yet: stop with a clear reason
};
static_assert(offsetof(PpuThreadContext, gpr) == 0, "JIT assumes gpr at offset 0");
static_assert(offsetof(PpuThreadContext, vr) % 16 == 0, "VMX registers must be 16-byte aligned");

// What runs a PPU thread: the JIT (phase 2), or an interpreter used to validate it.
class IPpuExecutor {
public:
    virtual ~IPpuExecutor() = default;
    // Runs until the thread exits (ctx.exit_requested) or a fatal guest fault.
    virtual void Run(PpuThreadContext& ctx) = 0;
};

}  // namespace cell::ppu
