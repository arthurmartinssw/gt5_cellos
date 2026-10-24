// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_process.cpp
#include "lv2/sys_process.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "common/log.h"
#include "ppu/ppu_interpreter.h"

namespace cell::lv2 {

s32 ProcessService::TtyWrite(u32 channel, u32 buffer, u32 length, u32 written_ptr) {
    if (channel > 15) return CELL_EINVAL;
    if (length && !memory_.Check(buffer, length, vm::kPageReadable)) return CELL_EFAULT;
    const std::string text(reinterpret_cast<const char*>(memory_.shadow() + buffer), length);
    state_.AppendTty(text);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    if (written_ptr && memory_.Check(written_ptr, 4, vm::kPageWritable)) memory_.Write<u32>(written_ptr, length);
    return CELL_OK;
}

s32 ProcessService::GetSdkVersion(u32 pid, u32 version_ptr) {
    (void)pid;
    if (!memory_.Check(version_ptr, 4, vm::kPageWritable)) return CELL_EFAULT;
    memory_.Write<u32>(version_ptr, state_.sdk_version);
    return CELL_OK;
}

s32 ProcessService::GetCurrentTime(u32 sec_ptr, u32 nsec_ptr) {
    if (!memory_.Check(sec_ptr, 8, vm::kPageWritable) || !memory_.Check(nsec_ptr, 8, vm::kPageWritable))
        return CELL_EFAULT;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const s64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    memory_.Write<u64>(sec_ptr, static_cast<u64>(ns / 1000000000));
    memory_.Write<u64>(nsec_ptr, static_cast<u64>(ns % 1000000000));
    return CELL_OK;
}

// Semantics follow RPCS3 (sys_process.cpp): the answer depends on the 256 MiB segment.
//   0x0, 0x1, 0x2 (main / user64k), 0xC (RSX local), 0xE (raw SPU): allowed;
//   0xD (stack): never — stacks are thread private;
//   0xF (SPU thread LS/SNR images): allowed for SPU threads, not for raw SPUs;
//   anything else (user1m, mmapper areas): allowed if something is mapped there.
s32 ProcessService::IsSpuLockLineReservationAddress(u32 addr, u64 flags) const {
    if (!flags || (flags & ~(SYS_MEMORY_ACCESS_RIGHT_RAW_SPU | SYS_MEMORY_ACCESS_RIGHT_SPU_THR))) return CELL_EINVAL;
    switch (addr >> 28) {
    case 0x0: case 0x1: case 0x2: case 0xC: case 0xE: return CELL_OK;
    case 0xD: return CELL_EPERM;
    case 0xF: return (flags & SYS_MEMORY_ACCESS_RIGHT_RAW_SPU) ? CELL_EPERM : CELL_OK;
    default: break;
    }
    // user1m always exists as an area; in the mmapper range a segment exists only once
    // something was mapped there (approximated by the queried page being mapped).
    if (vm::GuestMemory::AreaOf(addr) == vm::Area::User1M) return CELL_OK;
    return (memory_.Flags(addr) & vm::kPageMapped) ? CELL_OK : CELL_EINVAL;
}

s32 ProcessService::GetParamSfo(u32 buffer) {
    if (!memory_.Check(buffer, 0x40, vm::kPageWritable)) return CELL_EFAULT;
    if (state_.title_id.empty()) return CELL_ENOENT;
    memory_.Fill(buffer, 0, 0x40);
    memory_.CopyIn(buffer + 1, state_.title_id.data(), std::min<std::size_t>(state_.title_id.size(), 9));
    return CELL_OK;
}

void ProcessService::RegisterSyscalls(SyscallTable& table) {
    using C = ppu::PpuThreadContext;
    table.Register(SYS_PROCESS_GETPID, "sys_process_getpid", [](void*, C&) -> s32 { return static_cast<s32>(kProcessId); },
                   this);
    table.Register(
        SYS_PROCESS_EXIT, "_sys_process_exit",
        [](void*, C& ctx) -> s32 {
            ctx.exit_status = ctx.gpr[3];
            ctx.exit_kind = ppu::kExitProcess;
            ctx.exit_requested = true;
            return CELL_OK;
        },
        this);
    table.Register(
        SYS_PROCESS_GET_SDK_VERSION, "sys_process_get_sdk_version",
        [](void* self, C& ctx) -> s32 {
            return static_cast<ProcessService*>(self)->GetSdkVersion(static_cast<u32>(ctx.gpr[3]),
                                                                    static_cast<u32>(ctx.gpr[4]));
        },
        this);
    table.Register(
        SYS_PROCESS_IS_SPU_LOCK_LINE_RESERVATION_ADDRESS, "sys_process_is_spu_lock_line_reservation_address",
        [](void* self, C& ctx) -> s32 {
            return static_cast<ProcessService*>(self)->IsSpuLockLineReservationAddress(static_cast<u32>(ctx.gpr[3]), ctx.gpr[4]);
        },
        this);
    table.Register(
        SYS_PROCESS_GET_PARAMSFO, "_sys_process_get_paramsfo",
        [](void* self, C& ctx) -> s32 { return static_cast<ProcessService*>(self)->GetParamSfo(static_cast<u32>(ctx.gpr[3])); },
        this);
    table.Register(
        SYS_PPU_THREAD_EXIT, "sys_ppu_thread_exit",
        [](void*, C& ctx) -> s32 {
            ctx.exit_status = ctx.gpr[3];
            ctx.exit_kind = ppu::kExitThread;
            ctx.exit_requested = true;
            return CELL_OK;
        },
        this);
    table.Register(
        SYS_TIME_GET_CURRENT_TIME, "sys_time_get_current_time",
        [](void* self, C& ctx) -> s32 {
            return static_cast<ProcessService*>(self)->GetCurrentTime(static_cast<u32>(ctx.gpr[3]),
                                                                     static_cast<u32>(ctx.gpr[4]));
        },
        this);
    table.Register(
        SYS_TIME_GET_TIMEBASE_FREQUENCY, "sys_time_get_timebase_frequency",
        [](void*, C&) -> s32 { return static_cast<s32>(ppu::PpuInterpreter::kTimebaseFrequency); }, this);
    table.Register(
        SYS_TTY_WRITE, "sys_tty_write",
        [](void* self, C& ctx) -> s32 {
            return static_cast<ProcessService*>(self)->TtyWrite(static_cast<u32>(ctx.gpr[3]), static_cast<u32>(ctx.gpr[4]),
                                                               static_cast<u32>(ctx.gpr[5]), static_cast<u32>(ctx.gpr[6]));
        },
        this);
}

}  // namespace cell::lv2
