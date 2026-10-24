// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/kernel.h
//
// lv2 kernel core: object IDs, blocking waits and process termination.
//
// Concurrency model (phase 3):
//   * 1 PPU thread = 1 host std::thread running its own PpuInterpreter over the shared
//     PpuCodeCache and the shared 4 GiB arena. The primary thread runs on the caller's thread.
//   * A guest syscall that blocks (mutex lock, event receive, join, sleep) blocks the host
//     thread inside the syscall handler on a std::condition_variable owned by the lv2 object.
//     No guest scheduler: the host OS schedules. lv2 priorities are recorded, not enforced.
//   * Every lv2 object (Lv2Object) lives in ObjectTable as shared_ptr: a thread blocked on an
//     object keeps it alive even if another thread destroys its ID meanwhile.
//   * Process termination (sys_process_exit, exitspawn, a fatal fault on any thread):
//     RequestTermination() raises `terminating`, then locks each object and notify_all()s its
//     condition variables. Every wait predicate includes `terminating` and is evaluated under
//     the object's mutex, so no thread can miss the wake-up. Interpreters see the same flag
//     at their next instruction and stop with StopReason::Terminated.
//   * Lock order: ObjectTable mutex is never held while taking an object mutex; object
//     mutexes are never nested (cond waits use their mutex's lock, not a second one).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "hle/hle_registry.h"
#include "lv2/sys_process.h"
#include "lv2/syscall_table.h"
#include "lv2/vfs.h"
#include "memory/guest_memory.h"
#include "ppu/ppu_code_cache.h"
#include "ppu/ppu_interpreter.h"

namespace cell::lv2 {

class ThreadManager;
class SpuManager;
class MmapperService;
class PrxManager;
class FsService;
class TimerService;

// ID bases per object type (as RPCS3 numbers them, so logs can be compared).
inline constexpr u32 kIdPpuThread = 0x01000000;
inline constexpr u32 kIdMutex = 0x85000000;
inline constexpr u32 kIdCond = 0x86000000;
inline constexpr u32 kIdEventQueue = 0x8D000000;
inline constexpr u32 kIdEventPort = 0x0E000000;
inline constexpr u32 kIdLwMutex = 0x95000000;
inline constexpr u32 kIdSemaphore = 0x96000000;
inline constexpr u32 kIdLwCond = 0x97000000;
inline constexpr u32 kIdEventFlag = 0x98000000;

// lv2 sync attribute values.
inline constexpr u32 SYS_SYNC_FIFO = 1;
inline constexpr u32 SYS_SYNC_PRIORITY = 2;
inline constexpr u32 SYS_SYNC_PRIORITY_INHERIT = 3;
inline constexpr u32 SYS_SYNC_RETRY = 4;
inline constexpr u32 SYS_SYNC_RECURSIVE = 0x10;
inline constexpr u32 SYS_SYNC_NOT_RECURSIVE = 0x20;

struct Lv2Object {
    virtual ~Lv2Object() = default;
    // Wake every waiter so it can observe Kernel::terminating(). Must lock the object's mutex
    // before notifying (see the model above).
    virtual void WakeForTermination() {}
    // One line for the stall report (waiters, owner, pending events...); empty = not listed.
    // Must not block: implementations try_lock their mutex and say "busy" if they cannot.
    [[nodiscard]] virtual std::string Describe() { return {}; }
    u32 id = 0;
    std::string name;
};

class ObjectTable {
public:
    // Inserts and assigns obj->id = base + n.
    u32 Add(u32 base, std::shared_ptr<Lv2Object> obj);
    template <typename T>
    [[nodiscard]] std::shared_ptr<T> Get(u32 id) const {
        std::lock_guard lock(mutex_);
        const auto it = objects_.find(id);
        return it == objects_.end() ? nullptr : std::dynamic_pointer_cast<T>(it->second);
    }
    bool Remove(u32 id);
    [[nodiscard]] std::vector<std::shared_ptr<Lv2Object>> Snapshot() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<u32, std::shared_ptr<Lv2Object>> objects_;
    std::unordered_map<u32, u32> next_;  // per base
};

enum class WaitResult { Ready, TimedOut, Terminated };

// How the process ended (first cause wins).
struct ProcessEnd {
    bool ended = false;
    u8 kind = ppu::kExitProcess;  // ppu::ExitKind
    u64 status = 0;
    u32 thread_id = 0;            // thread that caused it
    bool fatal = false;           // a thread stopped on a fault / unimplemented instruction
    ppu::StopInfo stop;           // that thread's stop info when fatal
    ppu::PpuThreadContext context{};
    std::vector<u32> recent_pcs;
    std::vector<ppu::CallRecord> recent_calls;
    std::string message;          // human-readable cause (HLE abort, SPU runtime, ...)
};

class Kernel {
public:
    Kernel(vm::GuestMemory& memory, hle::HleRegistry& hle, SyscallTable& syscalls, ppu::PpuCodeCache& code,
           ProcessState& process);
    ~Kernel();
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    vm::GuestMemory& memory() noexcept { return memory_; }
    hle::HleRegistry& hle() noexcept { return hle_; }
    SyscallTable& syscalls() noexcept { return syscalls_; }
    ppu::PpuCodeCache& code() noexcept { return code_; }
    ProcessState& process() noexcept { return process_; }
    ObjectTable& objects() noexcept { return objects_; }
    ThreadManager& threads() noexcept { return *threads_; }
    SpuManager& spu() noexcept { return *spu_; }
    MmapperService& mmapper() noexcept { return *mmapper_; }
    PrxManager& prx() noexcept { return *prx_; }
    FsService& fs() noexcept { return *fs_; }
    TimerService& timers() noexcept { return *timers_; }
    Vfs& vfs() noexcept { return vfs_; }

    [[nodiscard]] const std::atomic<bool>& terminating() const noexcept { return terminating_; }
    [[nodiscard]] bool is_terminating() const noexcept { return terminating_.load(std::memory_order_acquire); }

    // First call records the cause; every call raises `terminating` and wakes all waiters.
    void RequestTermination(const ProcessEnd& cause);
    [[nodiscard]] ProcessEnd process_end() const;

    // Blocking wait on `cv` with `lock` held, until `ready()`, timeout (0 = infinite) or
    // termination. The predicate runs under the lock.
    template <typename Pred>
    WaitResult Wait(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, u64 timeout_us, Pred ready) {
        auto done = [&] { return ready() || is_terminating(); };
        if (!timeout_us) {
            cv.wait(lock, done);
        } else if (!cv.wait_for(lock, std::chrono::microseconds(timeout_us), done)) {
            return WaitResult::TimedOut;
        }
        return ready() ? WaitResult::Ready : WaitResult::Terminated;
    }

    // sys_timer_usleep: false if cut short by termination.
    bool Sleep(u64 usec);

    void RegisterSyscalls();

    // Process end: terminates (if not yet), joins every PPU and SPU host thread. Idempotent.
    void Shutdown();

    // Thread id of the PPU thread running on the calling host thread (0 if none).
    static u32 CurrentThreadId() noexcept;
    static void SetCurrentThreadId(u32 id) noexcept;

private:
    vm::GuestMemory& memory_;
    hle::HleRegistry& hle_;
    SyscallTable& syscalls_;
    ppu::PpuCodeCache& code_;
    ProcessState& process_;
    ObjectTable objects_;
    std::unique_ptr<ThreadManager> threads_;
    std::unique_ptr<SpuManager> spu_;
    std::unique_ptr<MmapperService> mmapper_;
    Vfs vfs_;
    std::unique_ptr<PrxManager> prx_;
    std::unique_ptr<FsService> fs_;  // after vfs_: open files refer to its mounts
    std::unique_ptr<TimerService> timers_;  // its thread pushes into event queues (objects_)

    std::atomic<bool> terminating_{false};
    mutable std::mutex end_mutex_;
    ProcessEnd end_;
    std::mutex sleep_mutex_;
    std::condition_variable sleep_cv_;
};

}  // namespace cell::lv2
