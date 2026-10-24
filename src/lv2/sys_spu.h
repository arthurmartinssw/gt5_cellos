// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_spu.h
//
// lv2 SPU thread groups and SPU threads. Syscalls (lv2 numbering, as in RPCS3's table):
//   165 sys_spu_thread_get_exit_status     166 sys_spu_thread_set_argument
//   169 sys_spu_initialize                 170 sys_spu_thread_group_create
//   171 sys_spu_thread_group_destroy       172 sys_spu_thread_initialize
//   173 sys_spu_thread_group_start         176 sys_spu_thread_group_yield
//   177 sys_spu_thread_group_terminate     178 sys_spu_thread_group_join
//   179 sys_spu_thread_group_set_priority  180 sys_spu_thread_group_get_priority
//   181 sys_spu_thread_write_ls            182 sys_spu_thread_read_ls
//   184 sys_spu_thread_write_snr           185 sys_spu_thread_group_connect_event
//   186 sys_spu_thread_group_disconnect_event
//   187 sys_spu_thread_set_spu_cfg         188 sys_spu_thread_get_spu_cfg
//   190 sys_spu_thread_write_spu_mb        191 sys_spu_thread_connect_event
//   192 sys_spu_thread_disconnect_event    193 sys_spu_thread_bind_queue
//   194 sys_spu_thread_unbind_queue
// Not implemented (stay ENOSYS with a named warning): raw SPUs (160-168), group
// suspend/resume (174/175).
//
// PS3 -> PC mapping:
//   * sys_spu_thread_initialize reserves a 1 MiB window in the spu_thread area of the guest
//     arena (0xF0000000+) and commits its first 256 KiB as the thread's local store; the rest
//     of the window is left inaccessible as a guard. The LS is ordinary host memory: the
//     SPU executor/JIT addresses it as ls[addr & 0x3FFFF].
//   * sys_spu_thread_group_start spawns one host std::thread per SPU thread. The host OS
//     schedules them next to the PPU threads; no hardware SPU is simulated.
//   * The SPU instruction stream is run by an ISpuExecutor created through the factory set
//     with SetExecutorFactory(). Phase 4 ships no executor (the SPU ISA decoder is the next
//     phase): starting a group then ends the process with an explicit report.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lv2/kernel.h"
#include "spu/spu_thread.h"

namespace cell::lv2 {

inline constexpr u32 SYS_SPU_INITIALIZE = 169;
inline constexpr u32 kPs3UsableSpus = 6;  // SPUs available to a game process
inline constexpr u32 kPs3MaxRawSpus = 5;

inline constexpr u32 kIdSpuThread = 0x02000000;
inline constexpr u32 kIdSpuThreadGroup = 0x04000000;

inline constexpr s32 SYS_SPU_THREAD_GROUP_TYPE_NORMAL = 0x00;
inline constexpr s32 SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM = 0x20;
inline constexpr u32 SYS_SPU_THREAD_EVENT_USER = 1;
inline constexpr u32 SYS_SPU_THREAD_GROUP_EVENT_RUN = 1;
inline constexpr u32 SYS_SPU_THREAD_GROUP_EVENT_EXCEPTION = 2;
inline constexpr u32 SYS_SPU_THREAD_GROUP_EVENT_SYSTEM_MODULE = 4;
inline constexpr u32 kMaxBoundSpuQueues = 32;

using SpuExecutorFactory = std::function<std::unique_ptr<spu::ISpuExecutor>(spu::SpuThread&)>;

class SpuManager {
public:
    explicit SpuManager(Kernel& kernel) noexcept : kernel_(kernel) {}
    ~SpuManager();
    SpuManager(const SpuManager&) = delete;
    SpuManager& operator=(const SpuManager&) = delete;

    s32 Initialize(u32 max_usable_spu, u32 max_raw_spu);

    struct Limits {
        bool initialized = false;
        u32 max_usable_spu = kPs3UsableSpus;
        u32 max_raw_spu = 0;
    };
    [[nodiscard]] Limits limits() const;

    // The SPU interpreter / JIT plugs in here. Without a factory, SPU code cannot run.
    void SetExecutorFactory(SpuExecutorFactory factory);
    [[nodiscard]] std::unique_ptr<spu::ISpuExecutor> CreateExecutor(spu::SpuThread& thread) const;

    // ---- operations behind the syscalls (also used by HLE and tests); return CELL_* ----
    struct GroupParams {
        u32 num = 1;
        s32 priority = 100;
        s32 type = SYS_SPU_THREAD_GROUP_TYPE_NORMAL;
        std::string name;
    };
    s32 CreateGroup(const GroupParams& params, u32* out_id);
    s32 DestroyGroup(u32 id);
    struct ThreadParams {
        u32 group = 0;
        u32 spu_num = 0;
        spu::SpuElfInfo image;
        u64 args[4] = {};
        u32 option = 0;
        std::string name;
    };
    s32 InitializeThread(const ThreadParams& params, u32* out_id);
    s32 StartGroup(u32 id);
    s32 TerminateGroup(u32 id, s32 value);
    s32 JoinGroup(u32 id, s32* cause, s32* status);

    [[nodiscard]] std::shared_ptr<spu::SpuThread> FindThread(u32 id) const;
    [[nodiscard]] std::shared_ptr<spu::SpuThreadGroup> FindGroup(u32 id) const;

    // Stops every running group and joins all SPU host threads (process end). Idempotent.
    void Shutdown();

    // One line per group/thread for the end-of-process report.
    [[nodiscard]] std::vector<std::string> Describe() const;

    void RegisterSyscalls(SyscallTable& table);

private:
    Kernel& kernel_;
    mutable std::mutex mutex_;
    Limits limits_;
    SpuExecutorFactory factory_;
    u32 spus_in_use_ = 0;  // sum of `num` over existing groups
    std::vector<std::shared_ptr<spu::SpuThreadGroup>> groups_;  // creation order, for reports
};

}  // namespace cell::lv2
