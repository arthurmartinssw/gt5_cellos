// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_process.h
//
// Process-level lv2 state and the first syscalls a PPU program issues while booting:
// console output, process exit, SDK version, time. Syscall numbers follow the lv2 table as
// reimplemented by RPCS3 (Emu/Cell/lv2/lv2.cpp); an unknown number is logged by
// SyscallTable::Dispatch and returns CELL_ENOSYS.
#pragma once

#include <mutex>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "lv2/syscall_table.h"
#include "memory/guest_memory.h"

namespace cell::lv2 {

inline constexpr u32 SYS_PROCESS_GETPID = 1;
inline constexpr u32 SYS_PROCESS_EXIT = 22;  // _sys_process_exit
inline constexpr u32 SYS_PROCESS_IS_SPU_LOCK_LINE_RESERVATION_ADDRESS = 14;
inline constexpr u32 SYS_PROCESS_GET_SDK_VERSION = 25;
inline constexpr u32 SYS_PROCESS_GET_PARAMSFO = 30;  // _sys_process_get_paramsfo

// Flags of sys_process_is_spu_lock_line_reservation_address.
inline constexpr u64 SYS_MEMORY_ACCESS_RIGHT_RAW_SPU = 0x1;
inline constexpr u64 SYS_MEMORY_ACCESS_RIGHT_SPU_THR = 0x2;
inline constexpr u32 SYS_PPU_THREAD_EXIT = 41;
inline constexpr u32 SYS_TIME_GET_CURRENT_TIME = 145;
inline constexpr u32 SYS_TIME_GET_TIMEBASE_FREQUENCY = 147;
inline constexpr u32 SYS_TTY_WRITE = 403;

inline constexpr u32 kProcessId = 0x01000500;  // value lv2 hands to the first game process

struct ProcessState {
    // Set once at boot, read-only afterwards.
    u32 sdk_version = 0;
    std::string title_id;  // from PARAM.SFO (or --title-id), e.g. "BCUS98114"; empty if unknown
    // The whole PARAM.SFO (cellGameGetParamString/Int): text entries and int32 entries by key.
    std::map<std::string, std::string> sfo_text;
    std::map<std::string, u32> sfo_int;
    bool disc_boot = true;  // booted from /dev_bdvd (cellGameBootCheck: DISC) or from /dev_hdd0/game (HDD)
    u32 tls_vaddr = 0;   // PT_TLS image used for every new thread's TLS block
    u32 tls_filesz = 0;
    u32 tls_memsz = 0;
    u32 primary_stack_size = 0x100000;

    // Mutable from any PPU thread: guarded by `mutex`.
    std::mutex mutex;
    bool exitspawn_requested = false;
    std::string exitspawn_path;
    std::vector<std::string> exitspawn_args;
    u32 exitspawn_prio = 0;
    u64 exitspawn_flags = 0;
    std::string tty_output;  // everything printed via sys_tty_write / _sys_printf

    void AppendTty(std::string_view text) {
        std::lock_guard lock(mutex);
        tty_output.append(text);
    }
};

class ProcessService {
public:
    ProcessService(vm::GuestMemory& memory, ProcessState& state) noexcept : memory_(memory), state_(state) {}
    void RegisterSyscalls(SyscallTable& table);

    s32 TtyWrite(u32 channel, u32 buffer, u32 length, u32 written_ptr);
    s32 GetSdkVersion(u32 pid, u32 version_ptr);
    s32 GetCurrentTime(u32 sec_ptr, u32 nsec_ptr);
    // syscall 14: may an SPU (thread or raw) take a lock-line reservation (getllar/putllc) at
    // `addr`? libsre asks this for the CellSpurs instance before creating the kernel group;
    // a failure here makes cellSpursInitialize give up.
    s32 IsSpuLockLineReservationAddress(u32 addr, u64 flags) const;
    // syscall 30: 0x40-byte PARAM.SFO summary, title id at +1.
    s32 GetParamSfo(u32 buffer);

private:
    vm::GuestMemory& memory_;
    ProcessState& state_;
};

}  // namespace cell::lv2
