// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/syscall_table.h
//
// lv2 system-call dispatch. The PPU issues `sc` with the syscall number in r11 and arguments
// in r3-r10; the result comes back in r3. The JIT translates `sc` into a call to Dispatch().
//
// PS4 -> PS3 note: bloodborne_pc has no syscall layer. Orbis games call libkernel functions
// (sceKernelMapDirectMemory...) and the runtime binds those *imports* by NID to host
// functions (runtime_memory_resolve -> exports[] table). On the PS3 both paths exist:
//   * syscalls (`sc`, this table), issued by the static liblv2 stubs linked into the EBOOT,
//   * imported library functions (libstub/FNID, hle/hle_registry.h).
#pragma once

#include <array>
#include <atomic>

#include "common/log.h"
#include "common/types.h"
#include "ppu/ppu_context.h"

namespace cell::lv2 {

// Handler: returns the lv2 result (CELL_OK or CELL_E*). `self` is the owning service.
using SyscallFn = s32 (*)(void* self, ppu::PpuThreadContext& ctx);

// Name of an lv2 syscall we do not implement yet (nullptr if unknown), for diagnostics only.
[[nodiscard]] const char* KnownSyscallName(u32 number) noexcept;

struct SyscallEntry {
    SyscallFn fn = nullptr;
    void* self = nullptr;
    const char* name = nullptr;
};

inline constexpr u32 kSyscallCount = 1024;

class SyscallTable {
public:
    void Register(u32 number, const char* name, SyscallFn fn, void* self) noexcept {
        if (number < kSyscallCount) table_[number] = {fn, self, name};
    }

    // Hot path: one bounds check, one indirect call. Result is sign-extended into r3.
    void Dispatch(ppu::PpuThreadContext& ctx) const noexcept {
        const u64 number = ctx.gpr[11];
        if (number < kSyscallCount && table_[number].fn) {
            const SyscallEntry& e = table_[number];
            ctx.gpr[3] = static_cast<u64>(static_cast<s64>(e.fn(e.self, ctx)));
            return;
        }
        // Warn once per number: games retry failing calls in loops.
        if (number >= kSyscallCount || !warned_[number].exchange(true, std::memory_order_relaxed)) {
            const char* name = number < kSyscallCount ? KnownSyscallName(static_cast<u32>(number)) : nullptr;
            LOG_WARN("lv2", "unimplemented syscall %llu (%s) at 0x%08x (r3=0x%llx r4=0x%llx r5=0x%llx r6=0x%llx, LR=0x%08llx), "
                     "returning CELL_ENOSYS",
                     static_cast<unsigned long long>(number), name ? name : "unknown", ctx.cia,
                     static_cast<unsigned long long>(ctx.gpr[3]), static_cast<unsigned long long>(ctx.gpr[4]),
                     static_cast<unsigned long long>(ctx.gpr[5]), static_cast<unsigned long long>(ctx.gpr[6]),
                     static_cast<unsigned long long>(ctx.lr));
        }
        ctx.gpr[3] = static_cast<u64>(static_cast<s64>(CELL_ENOSYS));
    }

    [[nodiscard]] const SyscallEntry& At(u32 number) const noexcept { return table_[number]; }

private:
    std::array<SyscallEntry, kSyscallCount> table_{};
    mutable std::array<std::atomic<bool>, kSyscallCount> warned_{};
};

}  // namespace cell::lv2
