// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/sys_prx_for_user.h
//
// sysPrxForUser: the user-side half of liblv2 that every PS3 executable imports (TLS setup,
// thread creation, process exit/exitspawn, lightweight mutexes/conds, small libc helpers,
// printf to the TTY). Same role as the libkernel exports bloodborne_pc implements in its
// runtime_*.c files.
//
// Phase 3: thread creation and lwmutex/lwcond are real, built on the lv2 kernel objects
// (lv2/sys_ppu_thread, lv2/sys_sync). On hardware the lwmutex fast path is user-space atomics
// with syscalls only on contention; here the whole operation is one host call, which is both
// simpler and faster than interpreting the guest fast path.
#pragma once

#include <atomic>

#include "hle/hle_registry.h"
#include "lv2/kernel.h"
#include "lv2/sys_process.h"
#include "memory/guest_memory.h"

namespace cell::hle {

class SysPrxForUser {
public:
    SysPrxForUser(vm::GuestMemory& memory, lv2::ProcessState& process, lv2::Kernel& kernel) noexcept
        : memory_(memory), process_(process), kernel_(kernel) {}
    void Register(HleRegistry& registry);

    vm::GuestMemory& memory() noexcept { return memory_; }
    lv2::ProcessState& process() noexcept { return process_; }
    lv2::Kernel& kernel() noexcept { return kernel_; }

private:
    vm::GuestMemory& memory_;
    lv2::ProcessState& process_;
    lv2::Kernel& kernel_;
};

}  // namespace cell::hle
