// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_sync.cpp
#include "lv2/sys_sync.h"

#include <algorithm>
#include <cstdio>

#include "common/log.h"

namespace cell::lv2 {

std::string ReadObjectName(const vm::GuestMemory& memory, u32 addr) {
    if (!memory.Check(addr, 8, vm::kPageReadable)) return {};
    const char* p = reinterpret_cast<const char*>(memory.shadow() + addr);
    return std::string(p, std::find(p, p + 8, '\0'));
}

// =======================================================================================
// Lv2Mutex
// =======================================================================================

std::string Lv2Mutex::Describe() {
    char line[160];
    const char* kind = mirror_addr ? "lwmutex" : "mutex";
    std::unique_lock lock(m, std::try_to_lock);
    if (!lock) std::snprintf(line, sizeof(line), "%s 0x%08x \"%s\": busy", kind, id, name.c_str());
    else if (!owner && !waiters) return {};  // idle: not interesting in a stall
    else
        std::snprintf(line, sizeof(line), "%s 0x%08x \"%s\"%s: owner 0x%08x (x%u), %u waiter(s)", kind, id, name.c_str(),
                      mirror_addr ? " (guest struct)" : "", owner, lock_count + 1, waiters);
    return line;
}

void Lv2Mutex::WakeForTermination() {
    std::lock_guard lock(m);
    cv.notify_all();
}

void Lv2Mutex::MirrorLocked() {
    if (!mirror_memory || !mirror_addr) return;
    // sys_lwmutex_t: +0 owner, +4 waiter, +8 attribute, +12 recursive_count, +16 sleep_queue
    mirror_memory->Write<u32>(mirror_addr + 0, owner ? owner : kLwmutexFree);
    mirror_memory->Write<u32>(mirror_addr + 4, waiters);
    mirror_memory->Write<u32>(mirror_addr + 12, lock_count);
}

s32 Lv2Mutex::Lock(Kernel& kernel, u32 tid, u64 timeout_us) {
    std::unique_lock lock(m);
    if (destroyed) return CELL_ESRCH;
    if (owner == tid) {
        if (!recursive) return CELL_EDEADLK;
        if (lock_count == 0xFFFFFFFFu) return CELL_EKRESOURCE;
        ++lock_count;
        MirrorLocked();
        return CELL_OK;
    }
    ++waiters;
    MirrorLocked();
    const WaitResult r = kernel.Wait(lock, cv, timeout_us, [&] { return owner == 0 || destroyed; });
    --waiters;
    if (r == WaitResult::Terminated) return CELL_ECANCELED;
    if (destroyed) return CELL_ESRCH;
    if (r == WaitResult::TimedOut) {
        MirrorLocked();
        return CELL_ETIMEDOUT;
    }
    owner = tid;
    lock_count = 0;
    MirrorLocked();
    return CELL_OK;
}

s32 Lv2Mutex::TryLock(u32 tid) {
    std::lock_guard lock(m);
    if (destroyed) return CELL_ESRCH;
    if (owner == tid) {
        if (!recursive) return CELL_EDEADLK;
        ++lock_count;
        MirrorLocked();
        return CELL_OK;
    }
    if (owner) return CELL_EBUSY;
    owner = tid;
    lock_count = 0;
    MirrorLocked();
    return CELL_OK;
}

s32 Lv2Mutex::Unlock(u32 tid) {
    std::lock_guard lock(m);
    if (destroyed) return CELL_ESRCH;
    if (owner != tid) return CELL_EPERM;
    if (lock_count) {
        --lock_count;
    } else {
        owner = 0;
        cv.notify_all();
    }
    MirrorLocked();
    return CELL_OK;
}

// =======================================================================================
// Lv2Cond
// =======================================================================================

std::string Lv2Cond::Describe() {
    if (!mutex) return {};
    char line[160];
    std::unique_lock lock(mutex->m, std::try_to_lock);
    if (!lock) std::snprintf(line, sizeof(line), "cond 0x%08x \"%s\": busy", id, name.c_str());
    else if (waiters.empty()) return {};
    else
        std::snprintf(line, sizeof(line), "cond 0x%08x \"%s\" on 0x%08x: %zu waiter(s), first 0x%08x", id, name.c_str(),
                      mutex->id, waiters.size(), waiters.front()->tid);
    return line;
}

namespace {
void Remember(std::array<u32, Lv2Cond::kHistory>& history, u32 tid) {
    if (!tid) return;
    for (u32& slot : history) {
        if (slot == tid) return;
        if (!slot) {
            slot = tid;
            return;
        }
    }
}
std::string HistoryText(const std::array<u32, Lv2Cond::kHistory>& history) {
    std::string out;
    char id[16];
    for (u32 tid : history)
        if (tid) {
            std::snprintf(id, sizeof(id), "%s0x%08x", out.empty() ? "" : " ", tid);
            out += id;
        }
    return out.empty() ? "none" : out;
}
}  // namespace

void Lv2Cond::SetCreator(u32 tid, u32 lr) {
    creator = tid;
    creator_lr = lr;
    created_us = ppu::PpuInterpreter::SystemTimeMicroseconds();
}

std::vector<u32> Lv2Cond::Signalers() {
    std::vector<u32> out;
    if (!mutex) return out;
    std::unique_lock lock(mutex->m, std::try_to_lock);
    if (!lock) return out;
    for (u32 tid : signaler_history)
        if (tid) out.push_back(tid);
    return out;
}

std::string Lv2Cond::Stats() {
    char line[256];
    if (!mutex) return {};
    std::unique_lock lock(mutex->m, std::try_to_lock);
    if (!lock) {
        std::snprintf(line, sizeof(line), "cond 0x%08x \"%s\": busy", id, name.c_str());
        return line;
    }
    const u64 now = ppu::PpuInterpreter::SystemTimeMicroseconds();
    char last[64] = "never signaled";
    if (signals)
        std::snprintf(last, sizeof(last), "last signal %.1f ms ago by 0x%08x",
                      static_cast<double>(now - std::min(now, last_signal_us)) / 1000.0, last_signaler);
    std::snprintf(line, sizeof(line),
                  "cond 0x%08x \"%s\" on mutex 0x%08x (owner 0x%08x): %zu waiter(s), %llu wait(s), %llu signal(s) "
                  "(%llu found no waiter), %s%s",
                  id, name.c_str(), mutex->id, mutex->owner, waiters.size(), static_cast<unsigned long long>(waits),
                  static_cast<unsigned long long>(signals), static_cast<unsigned long long>(unmatched_signals), last,
                  destroyed ? ", destroyed" : "");
    std::string out = line;
    std::snprintf(line, sizeof(line), "; created by 0x%08x (lr 0x%08x) %.1f ms ago; ever waited on by: %s; ever signaled by: ", creator,
                  creator_lr, static_cast<double>(now - std::min(now, created_us)) / 1000.0, HistoryText(waiter_history).c_str());
    out += line;
    out += HistoryText(signaler_history);
    return out;
}

void Lv2Cond::WakeForTermination() {
    std::lock_guard lock(mutex->m);
    cv.notify_all();
}

s32 Lv2Cond::Wait(Kernel& kernel, u32 tid, u64 timeout_us) {
    Lv2Mutex& mx = *mutex;
    std::unique_lock lock(mx.m);
    if (destroyed) return CELL_ESRCH;
    if (mx.owner != tid) return CELL_EPERM;

    // Release the mutex completely (recursion included) and start waiting, atomically.
    const u32 saved_count = mx.lock_count;
    mx.owner = 0;
    mx.lock_count = 0;
    mx.cv.notify_all();
    mx.MirrorLocked();

    Waiter self{tid, false};
    waiters.push_back(&self);
    ++waits;
    Remember(waiter_history, tid);
    const WaitResult r = kernel.Wait(lock, cv, timeout_us, [&] { return self.signaled; });
    if (!self.signaled) waiters.remove(&self);

    // lv2 returns with the mutex held, also after a timeout.
    ++mx.waiters;
    mx.MirrorLocked();
    const WaitResult r2 = kernel.Wait(lock, mx.cv, 0, [&] { return mx.owner == 0; });
    --mx.waiters;
    if (r2 == WaitResult::Terminated) return CELL_ECANCELED;
    mx.owner = tid;
    mx.lock_count = saved_count;
    mx.MirrorLocked();
    if (self.signaled) return CELL_OK;
    return r == WaitResult::TimedOut ? CELL_ETIMEDOUT : CELL_ECANCELED;
}

s32 Lv2Cond::Signal(u32 signaler) {
    std::lock_guard lock(mutex->m);
    if (destroyed) return CELL_ESRCH;
    ++signals;
    last_signal_us = ppu::PpuInterpreter::SystemTimeMicroseconds();
    last_signaler = signaler;
    Remember(signaler_history, signaler);
    if (waiters.empty()) ++unmatched_signals;
    if (!waiters.empty()) {
        waiters.front()->signaled = true;
        waiters.pop_front();
        cv.notify_all();
    }
    return CELL_OK;
}

s32 Lv2Cond::SignalAll(u32 signaler) {
    std::lock_guard lock(mutex->m);
    if (destroyed) return CELL_ESRCH;
    ++signals;
    last_signal_us = ppu::PpuInterpreter::SystemTimeMicroseconds();
    last_signaler = signaler;
    Remember(signaler_history, signaler);
    if (waiters.empty()) ++unmatched_signals;
    for (Waiter* w : waiters) w->signaled = true;
    waiters.clear();
    cv.notify_all();
    return CELL_OK;
}

s32 Lv2Cond::SignalTo(u32 tid, u32 signaler) {
    std::lock_guard lock(mutex->m);
    if (destroyed) return CELL_ESRCH;
    ++signals;
    last_signal_us = ppu::PpuInterpreter::SystemTimeMicroseconds();
    last_signaler = signaler;
    Remember(signaler_history, signaler);
    const auto it = std::find_if(waiters.begin(), waiters.end(), [&](Waiter* w) { return w->tid == tid; });
    if (it == waiters.end()) return CELL_EPERM;  // that thread is not waiting here
    (*it)->signaled = true;
    waiters.erase(it);
    cv.notify_all();
    return CELL_OK;
}

// =======================================================================================
// Lv2Semaphore
// =======================================================================================

std::string Lv2Semaphore::Describe() {
    char line[160];
    std::unique_lock lock(m, std::try_to_lock);
    if (!lock) std::snprintf(line, sizeof(line), "semaphore 0x%08x \"%s\": busy", id, name.c_str());
    else if (!waiters) return {};
    else std::snprintf(line, sizeof(line), "semaphore 0x%08x \"%s\": count %d/%d, %u waiter(s)", id, name.c_str(), count, max, waiters);
    return line;
}

void Lv2Semaphore::WakeForTermination() {
    std::lock_guard lock(m);
    cv.notify_all();
}

s32 Lv2Semaphore::Wait(Kernel& kernel, u64 timeout_us) {
    std::unique_lock lock(m);
    if (destroyed) return CELL_ESRCH;
    ++waiters;
    const WaitResult r = kernel.Wait(lock, cv, timeout_us, [&] { return count > 0 || destroyed; });
    --waiters;
    if (r == WaitResult::Terminated) return CELL_ECANCELED;
    if (destroyed) return CELL_ESRCH;
    if (r == WaitResult::TimedOut) return CELL_ETIMEDOUT;
    --count;
    return CELL_OK;
}

s32 Lv2Semaphore::TryWait() {
    std::lock_guard lock(m);
    if (destroyed) return CELL_ESRCH;
    if (count <= 0) return CELL_EBUSY;
    --count;
    return CELL_OK;
}

s32 Lv2Semaphore::Post(s32 n) {
    if (n < 0) return CELL_EINVAL;
    std::lock_guard lock(m);
    if (destroyed) return CELL_ESRCH;
    if (static_cast<s64>(count) + n > max) return CELL_EBUSY;
    count += n;
    cv.notify_all();
    return CELL_OK;
}

// =======================================================================================
// Lv2EventFlag
// =======================================================================================

namespace {
bool ValidFlagMode(u32 mode) {
    const u32 wait = mode & 0xF, clear = mode & 0xF0;
    return (wait == SYS_EVENT_FLAG_WAIT_AND || wait == SYS_EVENT_FLAG_WAIT_OR) &&
           (clear == 0 || clear == SYS_EVENT_FLAG_WAIT_CLEAR || clear == SYS_EVENT_FLAG_WAIT_CLEAR_ALL) &&
           !(mode & ~0xFFu);
}
bool FlagMatches(u64 pattern, u64 bits, u32 mode) {
    return (mode & SYS_EVENT_FLAG_WAIT_AND) ? (pattern & bits) == bits : (pattern & bits) != 0;
}
void FlagConsume(u64& pattern, u64 bits, u32 mode) {
    if (mode & SYS_EVENT_FLAG_WAIT_CLEAR) pattern &= ~bits;
    else if (mode & SYS_EVENT_FLAG_WAIT_CLEAR_ALL) pattern = 0;
}
}  // namespace

std::string Lv2EventFlag::Describe() {
    char line[160];
    std::unique_lock lock(m, std::try_to_lock);
    if (!lock) std::snprintf(line, sizeof(line), "event flag 0x%08x \"%s\": busy", id, name.c_str());
    else if (!waiters) return {};
    else
        std::snprintf(line, sizeof(line), "event flag 0x%08x \"%s\": pattern 0x%016llx, %u waiter(s)", id, name.c_str(),
                      static_cast<unsigned long long>(pattern), waiters);
    return line;
}

void Lv2EventFlag::WakeForTermination() {
    std::lock_guard lock(m);
    cv.notify_all();
}

s32 Lv2EventFlag::Wait(Kernel& kernel, u64 bits, u32 mode, u64* result, u64 timeout_us) {
    if (!bits || !ValidFlagMode(mode)) return CELL_EINVAL;
    std::unique_lock lock(m);
    if (destroyed) return CELL_ESRCH;
    if (FlagMatches(pattern, bits, mode)) {
        *result = pattern;
        FlagConsume(pattern, bits, mode);
        return CELL_OK;
    }
    if (type == SYS_SYNC_WAITER_SINGLE && waiters) return CELL_EPERM;
    const u64 generation = cancel_generation;
    ++waiters;
    const WaitResult r = kernel.Wait(lock, cv, timeout_us, [&] {
        return FlagMatches(pattern, bits, mode) || cancel_generation != generation || destroyed;
    });
    --waiters;
    *result = pattern;
    if (r == WaitResult::Terminated || cancel_generation != generation) return CELL_ECANCELED;
    if (destroyed) return CELL_ESRCH;
    if (r == WaitResult::TimedOut) return CELL_ETIMEDOUT;
    FlagConsume(pattern, bits, mode);
    return CELL_OK;
}

s32 Lv2EventFlag::TryWait(u64 bits, u32 mode, u64* result) {
    if (!bits || !ValidFlagMode(mode)) return CELL_EINVAL;
    std::lock_guard lock(m);
    if (destroyed) return CELL_ESRCH;
    *result = pattern;
    if (!FlagMatches(pattern, bits, mode)) return CELL_EBUSY;
    FlagConsume(pattern, bits, mode);
    return CELL_OK;
}

void Lv2EventFlag::Set(u64 bits) {
    std::lock_guard lock(m);
    pattern |= bits;
    cv.notify_all();
}

void Lv2EventFlag::Clear(u64 bits) {
    std::lock_guard lock(m);
    pattern &= bits;  // lv2: clear keeps the bits that are SET in the argument
}

u32 Lv2EventFlag::Cancel() {
    std::lock_guard lock(m);
    const u32 n = waiters;
    ++cancel_generation;
    cv.notify_all();
    return n;
}

// =======================================================================================
// Syscalls
// =======================================================================================

namespace {

using C = ppu::PpuThreadContext;
Kernel& K(void* p) { return *static_cast<Kernel*>(p); }
u32 A32(const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); }

template <typename T>
s32 WithObject(Kernel& k, u32 id, auto&& fn) {
    const auto obj = k.objects().Get<T>(id);
    if (!obj) return CELL_ESRCH;
    return fn(*obj);
}

bool Writable(Kernel& k, u32 addr, u32 size) { return k.memory().Check(addr, size, vm::kPageWritable); }
bool Readable(Kernel& k, u32 addr, u32 size) { return k.memory().Check(addr, size, vm::kPageReadable); }

}  // namespace

void RegisterSyncSyscalls(Kernel& kernel) {
    SyscallTable& t = kernel.syscalls();

    // ------------------------------- mutex -------------------------------
    t.Register(100, "sys_mutex_create", [](void* p, C& c) -> s32 {
        // (u32* mutex_id, sys_mutex_attribute_t* attr): protocol +0, recursive +4,
        // pshared +8, adaptive +0xC, ipc_key +0x10, flags +0x18, name[8] +0x20
        Kernel& k = K(p);
        const u32 out = A32(c, 0), attr = A32(c, 1);
        if (!Writable(k, out, 4) || !Readable(k, attr, 0x28)) return CELL_EFAULT;
        const u32 protocol = k.memory().Read<u32>(attr), recursive = k.memory().Read<u32>(attr + 4);
        if (protocol < SYS_SYNC_FIFO || protocol > SYS_SYNC_RETRY) return CELL_EINVAL;
        if (recursive != SYS_SYNC_RECURSIVE && recursive != SYS_SYNC_NOT_RECURSIVE) return CELL_EINVAL;
        auto mx = std::make_shared<Lv2Mutex>();
        mx->protocol = protocol;
        mx->recursive = recursive == SYS_SYNC_RECURSIVE;
        mx->name = ReadObjectName(k.memory(), attr + 0x20);
        const u32 id = k.objects().Add(kIdMutex, mx);
        k.memory().Write<u32>(out, id);
        LOG_TRACE("lv2", "sys_mutex_create \"%s\" -> 0x%08x%s", mx->name.c_str(), id, mx->recursive ? " (recursive)" : "");
        return CELL_OK;
    }, &kernel);
    t.Register(101, "sys_mutex_destroy", [](void* p, C& c) -> s32 {
        Kernel& k = K(p);
        const u32 id = A32(c, 0);
        const s32 r = WithObject<Lv2Mutex>(k, id, [](Lv2Mutex& mx) -> s32 {
            std::lock_guard lock(mx.m);
            if (mx.owner || mx.waiters) return CELL_EBUSY;
            if (mx.cond_refs) return CELL_EPERM;
            mx.destroyed = true;
            return CELL_OK;
        });
        if (r == CELL_OK) k.objects().Remove(id);
        return r;
    }, &kernel);
    t.Register(102, "sys_mutex_lock", [](void* p, C& c) -> s32 {
        return WithObject<Lv2Mutex>(K(p), A32(c, 0), [&](Lv2Mutex& mx) { return mx.Lock(K(p), c.thread_id, c.gpr[4]); });
    }, &kernel);
    t.Register(103, "sys_mutex_trylock", [](void* p, C& c) -> s32 {
        return WithObject<Lv2Mutex>(K(p), A32(c, 0), [&](Lv2Mutex& mx) { return mx.TryLock(c.thread_id); });
    }, &kernel);
    t.Register(104, "sys_mutex_unlock", [](void* p, C& c) -> s32 {
        return WithObject<Lv2Mutex>(K(p), A32(c, 0), [&](Lv2Mutex& mx) { return mx.Unlock(c.thread_id); });
    }, &kernel);

    // ------------------------------- cond -------------------------------
    t.Register(105, "sys_cond_create", [](void* p, C& c) -> s32 {
        // (u32* cond_id, u32 mutex_id, sys_cond_attribute_t* attr): pshared +0, flags +4,
        // ipc_key +8, name[8] +0x10
        Kernel& k = K(p);
        const u32 out = A32(c, 0), attr = A32(c, 2);
        if (!Writable(k, out, 4) || !Readable(k, attr, 0x18)) return CELL_EFAULT;
        const auto mx = k.objects().Get<Lv2Mutex>(A32(c, 1));
        if (!mx || mx->mirror_addr) return CELL_ESRCH;  // must be a sys_mutex, not an lwmutex
        auto cond = std::make_shared<Lv2Cond>();
        cond->mutex = mx;
        cond->name = ReadObjectName(k.memory(), attr + 0x10);
        cond->SetCreator(c.thread_id, static_cast<u32>(c.lr));
        {
            std::lock_guard lock(mx->m);
            ++mx->cond_refs;
        }
        k.memory().Write<u32>(out, k.objects().Add(kIdCond, cond));
        return CELL_OK;
    }, &kernel);
    t.Register(106, "sys_cond_destroy", [](void* p, C& c) -> s32 {
        Kernel& k = K(p);
        const u32 id = A32(c, 0);
        const s32 r = WithObject<Lv2Cond>(k, id, [](Lv2Cond& cond) -> s32 {
            std::lock_guard lock(cond.mutex->m);
            if (!cond.waiters.empty()) return CELL_EBUSY;
            cond.destroyed = true;
            --cond.mutex->cond_refs;
            return CELL_OK;
        });
        if (r == CELL_OK) k.objects().Remove(id);
        return r;
    }, &kernel);
    t.Register(107, "sys_cond_wait", [](void* p, C& c) -> s32 {
        return WithObject<Lv2Cond>(K(p), A32(c, 0), [&](Lv2Cond& cond) { return cond.Wait(K(p), c.thread_id, c.gpr[4]); });
    }, &kernel);
    t.Register(108, "sys_cond_signal", [](void* p, C& c) -> s32 {
        return WithObject<Lv2Cond>(K(p), A32(c, 0), [&](Lv2Cond& cond) { return cond.Signal(c.thread_id); });
    }, &kernel);
    t.Register(109, "sys_cond_signal_all", [](void* p, C& c) -> s32 {
        return WithObject<Lv2Cond>(K(p), A32(c, 0), [&](Lv2Cond& cond) { return cond.SignalAll(c.thread_id); });
    }, &kernel);
    t.Register(110, "sys_cond_signal_to", [](void* p, C& c) -> s32 {
        return WithObject<Lv2Cond>(K(p), A32(c, 0), [&](Lv2Cond& cond) { return cond.SignalTo(A32(c, 1), c.thread_id); });
    }, &kernel);

    // ------------------------------- semaphore -------------------------------
    t.Register(90, "sys_semaphore_create", [](void* p, C& c) -> s32 {
        // (u32* sem_id, sys_semaphore_attribute_t* attr, s32 initial, s32 max): protocol +0,
        // pshared +4, ipc_key +8, flags +0x10, name[8] +0x18
        Kernel& k = K(p);
        const u32 out = A32(c, 0), attr = A32(c, 1);
        const s32 initial = static_cast<s32>(c.gpr[5]), max = static_cast<s32>(c.gpr[6]);
        if (!Writable(k, out, 4) || !Readable(k, attr, 0x20)) return CELL_EFAULT;
        if (max <= 0 || initial < 0 || initial > max) return CELL_EINVAL;
        auto sem = std::make_shared<Lv2Semaphore>();
        sem->protocol = k.memory().Read<u32>(attr);
        sem->count = initial;
        sem->max = max;
        sem->name = ReadObjectName(k.memory(), attr + 0x18);
        k.memory().Write<u32>(out, k.objects().Add(kIdSemaphore, sem));
        return CELL_OK;
    }, &kernel);
    t.Register(91, "sys_semaphore_destroy", [](void* p, C& c) -> s32 {
        Kernel& k = K(p);
        const u32 id = A32(c, 0);
        const s32 r = WithObject<Lv2Semaphore>(k, id, [](Lv2Semaphore& sem) -> s32 {
            std::lock_guard lock(sem.m);
            if (sem.waiters) return CELL_EBUSY;
            sem.destroyed = true;
            return CELL_OK;
        });
        if (r == CELL_OK) k.objects().Remove(id);
        return r;
    }, &kernel);
    t.Register(92, "sys_semaphore_wait", [](void* p, C& c) -> s32 {
        return WithObject<Lv2Semaphore>(K(p), A32(c, 0), [&](Lv2Semaphore& sem) { return sem.Wait(K(p), c.gpr[4]); });
    }, &kernel);
    t.Register(93, "sys_semaphore_trywait", [](void* p, C& c) -> s32 {
        return WithObject<Lv2Semaphore>(K(p), A32(c, 0), [](Lv2Semaphore& sem) { return sem.TryWait(); });
    }, &kernel);
    t.Register(94, "sys_semaphore_post", [](void* p, C& c) -> s32 {
        return WithObject<Lv2Semaphore>(K(p), A32(c, 0), [&](Lv2Semaphore& sem) { return sem.Post(static_cast<s32>(c.gpr[4])); });
    }, &kernel);
    t.Register(114, "sys_semaphore_get_value", [](void* p, C& c) -> s32 {
        Kernel& k = K(p);
        const u32 out = A32(c, 1);
        if (!Writable(k, out, 4)) return CELL_EFAULT;
        return WithObject<Lv2Semaphore>(k, A32(c, 0), [&](Lv2Semaphore& sem) {
            std::lock_guard lock(sem.m);
            k.memory().Write<u32>(out, static_cast<u32>(sem.count));
            return CELL_OK;
        });
    }, &kernel);

    // ------------------------------- event flag -------------------------------
    t.Register(82, "sys_event_flag_create", [](void* p, C& c) -> s32 {
        // (u32* id, sys_event_flag_attribute_t* attr, u64 init): protocol +0, pshared +4,
        // ipc_key +8, flags +0x10, type +0x14, name[8] +0x18
        Kernel& k = K(p);
        const u32 out = A32(c, 0), attr = A32(c, 1);
        if (!Writable(k, out, 4) || !Readable(k, attr, 0x20)) return CELL_EFAULT;
        const s32 type = static_cast<s32>(k.memory().Read<u32>(attr + 0x14));
        if (type != SYS_SYNC_WAITER_SINGLE && type != SYS_SYNC_WAITER_MULTIPLE) return CELL_EINVAL;
        auto ef = std::make_shared<Lv2EventFlag>();
        ef->protocol = k.memory().Read<u32>(attr);
        ef->type = type;
        ef->pattern = c.gpr[5];
        ef->name = ReadObjectName(k.memory(), attr + 0x18);
        k.memory().Write<u32>(out, k.objects().Add(kIdEventFlag, ef));
        return CELL_OK;
    }, &kernel);
    t.Register(83, "sys_event_flag_destroy", [](void* p, C& c) -> s32 {
        Kernel& k = K(p);
        const u32 id = A32(c, 0);
        const s32 r = WithObject<Lv2EventFlag>(k, id, [](Lv2EventFlag& ef) -> s32 {
            std::lock_guard lock(ef.m);
            if (ef.waiters) return CELL_EBUSY;
            ef.destroyed = true;
            return CELL_OK;
        });
        if (r == CELL_OK) k.objects().Remove(id);
        return r;
    }, &kernel);
    t.Register(85, "sys_event_flag_wait", [](void* p, C& c) -> s32 {
        // (id, u64 bitptn, u32 mode, u64* result, u64 timeout)
        Kernel& k = K(p);
        const u32 out = A32(c, 3);
        if (out && !Writable(k, out, 8)) return CELL_EFAULT;
        u64 result = 0;
        const s32 r = WithObject<Lv2EventFlag>(k, A32(c, 0), [&](Lv2EventFlag& ef) {
            return ef.Wait(k, c.gpr[4], A32(c, 2), &result, c.gpr[7]);
        });
        if (out && r != CELL_ESRCH && r != CELL_EINVAL) k.memory().Write<u64>(out, result);
        return r;
    }, &kernel);
    t.Register(86, "sys_event_flag_trywait", [](void* p, C& c) -> s32 {
        Kernel& k = K(p);
        const u32 out = A32(c, 3);
        if (out && !Writable(k, out, 8)) return CELL_EFAULT;
        u64 result = 0;
        const s32 r = WithObject<Lv2EventFlag>(k, A32(c, 0), [&](Lv2EventFlag& ef) {
            return ef.TryWait(c.gpr[4], A32(c, 2), &result);
        });
        if (out && (r == CELL_OK || r == CELL_EBUSY)) k.memory().Write<u64>(out, result);
        return r;
    }, &kernel);
    t.Register(87, "sys_event_flag_set", [](void* p, C& c) -> s32 {
        return WithObject<Lv2EventFlag>(K(p), A32(c, 0), [&](Lv2EventFlag& ef) {
            ef.Set(c.gpr[4]);
            return CELL_OK;
        });
    }, &kernel);
    t.Register(118, "sys_event_flag_clear", [](void* p, C& c) -> s32 {
        return WithObject<Lv2EventFlag>(K(p), A32(c, 0), [&](Lv2EventFlag& ef) {
            ef.Clear(c.gpr[4]);
            return CELL_OK;
        });
    }, &kernel);
    t.Register(132, "sys_event_flag_cancel", [](void* p, C& c) -> s32 {
        Kernel& k = K(p);
        const u32 out = A32(c, 1);
        if (out && !Writable(k, out, 4)) return CELL_EFAULT;
        return WithObject<Lv2EventFlag>(k, A32(c, 0), [&](Lv2EventFlag& ef) {
            const u32 n = ef.Cancel();
            if (out) k.memory().Write<u32>(out, n);
            return CELL_OK;
        });
    }, &kernel);
    t.Register(139, "sys_event_flag_get", [](void* p, C& c) -> s32 {
        Kernel& k = K(p);
        const u32 out = A32(c, 1);
        if (!Writable(k, out, 8)) return CELL_EFAULT;
        return WithObject<Lv2EventFlag>(k, A32(c, 0), [&](Lv2EventFlag& ef) {
            std::lock_guard lock(ef.m);
            k.memory().Write<u64>(out, ef.pattern);
            return CELL_OK;
        });
    }, &kernel);
}

}  // namespace cell::lv2
