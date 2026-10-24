// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/stall_monitor.cpp
#include "lv2/stall_monitor.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>

#include "common/host_thread.h"
#include "hle/hle_registry.h"
#include "lv2/sys_event.h"
#include "lv2/sys_ppu_thread.h"
#include "lv2/sys_spu.h"
#include "lv2/sys_sync.h"
#include "lv2/sys_timer.h"
#include "ppu/ppu_disasm.h"

namespace cell::lv2 {

StallMonitor::StallMonitor(Kernel& kernel, u32 stall_seconds, u32 periodic_seconds)
    : kernel_(kernel), stall_seconds_(stall_seconds), periodic_seconds_(periodic_seconds) {}

StallMonitor::~StallMonitor() { Stop(); }

void StallMonitor::AddSection(std::string title, Section section) { sections_.emplace_back(std::move(title), std::move(section)); }

void StallMonitor::Start() {
    if (thread_.joinable() || (!stall_seconds_ && !periodic_seconds_)) return;
    thread_ = std::thread(&StallMonitor::Main, this);
}

void StallMonitor::Stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

u64 StallMonitor::Progress() const {
    u64 sum = 0;
    for (const auto& t : kernel_.threads().Snapshot()) sum += t.live_instructions + t.live_calls;
    return sum;
}

namespace {

std::string CallName(Kernel& kernel, u8 kind, u32 id) {
    if (kind == ppu::PpuLiveStatus::kInSyscall) {
        const char* name = id < kSyscallCount ? kernel.syscalls().At(id).name : nullptr;
        if (!name && id < kSyscallCount) name = KnownSyscallName(id);
        char text[96];
        std::snprintf(text, sizeof(text), "syscall %u %s", id, name ? name : "(unknown)");
        return text;
    }
    if (id < kernel.hle().size()) {
        const hle::HleFunction& f = kernel.hle().At(id);
        return "HLE " + f.module + "::" + (f.name.empty() ? "?" : f.name);
    }
    return "HLE #" + std::to_string(id);
}

// Number of real arguments of the blocking calls we decode (the report used to print r3..r6
// for every call; registers past the arity are leftovers, not arguments).
int Arity(Kernel& kernel, u8 kind, u32 id) {
    if (kind == ppu::PpuLiveStatus::kInSyscall) {
        switch (id) {
        case 141: case 142: return 1;                  // sys_timer_usleep / sleep
        case 102: case 107: case 92: case 44: return 2;  // mutex_lock, cond_wait, semaphore_wait, thread_join
        case 130: case 178: return 3;                  // event_queue_receive, spu group join
        default: return 4;
        }
    }
    if (id < kernel.hle().size()) {
        const std::string& n = kernel.hle().At(id).name;
        if (n == "sys_lwcond_wait" || n == "sys_lwmutex_lock") return 2;
        if (n == "sys_mempool_allocate_block") return 1;
        if (n == "gcm_interrupt_wait") return 0;
    }
    return 4;
}

// "BLOCKED in sys_lwcond_wait(0x5ffc8080)", "running", "exited" — one thread's state, short.
std::string ThreadState(Kernel& k, const ThreadManager::ThreadInfo& t) {
    if (t.state == PpuThread::State::Exited) return "exited";
    if (t.state == PpuThread::State::Created) return "created, not started";
    char text[160];
    switch (t.live_kind) {
    case ppu::PpuLiveStatus::kRunning: return "RUNNING guest code";
    case ppu::PpuLiveStatus::kInPageFault: return "suspended on a page fault";
    default:
        std::snprintf(text, sizeof(text), "BLOCKED in %s(0x%" PRIx64 ")", CallName(k, t.live_kind, t.live_call).c_str(), t.live_args[0]);
        return text;
    }
}

// "who should wake this condition": the threads that signaled it before, and what they do now.
void AppendSignalers(Kernel& k, Lv2Cond& cond, const std::vector<ThreadManager::ThreadInfo>& all, std::vector<std::string>& out) {
    const std::vector<u32> signalers = cond.Signalers();
    if (signalers.empty()) {
        out.emplace_back("expected producer: unknown — no thread has ever signaled this condition (the producer is not a "
                         "thread that already used it; see the creator and the calls before the wait)");
        return;
    }
    for (u32 tid : signalers) {
        std::string state = "no longer registered (exited and reaped)";
        std::string name;
        for (const auto& t : all)
            if (t.id == tid) {
                state = ThreadState(k, t);
                name = t.name;
            }
        char line[256];
        std::snprintf(line, sizeof(line), "past signaler 0x%08x%s%s: %s", tid, name.empty() ? "" : " ", name.c_str(), state.c_str());
        out.emplace_back(line);
    }
}

std::string RecentCallText(Kernel& k, const ppu::CallRecord& r) {
    char text[224];
    const u8 kind = r.syscall ? ppu::PpuLiveStatus::kInSyscall : ppu::PpuLiveStatus::kInHle;
    std::snprintf(text, sizeof(text), "%s(0x%" PRIx64 ", 0x%" PRIx64 ") -> 0x%" PRIx64 " [lr 0x%08x]", CallName(k, kind, r.id).c_str(),
                  r.args[0], r.args[1], r.result, r.pc);
    return text;
}

std::string Timeout(u64 us) {
    if (!us) return "no timeout";
    char t[48];
    std::snprintf(t, sizeof(t), "timeout %" PRIu64 " us", us);
    return t;
}

// sys_lwmutex_t at `ea` (validated): guest-visible owner and the lv2 mutex behind it.
std::string LwMutexLine(Kernel& k, u32 ea) {
    vm::GuestMemory& m = k.memory();
    char line[256];
    if (!ea || !m.Check(ea, 24, vm::kPageReadable)) {
        std::snprintf(line, sizeof(line), "lwmutex 0x%08x: not readable guest memory", ea);
        return line;
    }
    const u32 owner = m.Read<u32>(ea), waiter = m.Read<u32>(ea + 4), id = m.Read<u32>(ea + 16);
    std::string lv2 = "no lv2 object";
    if (const auto mx = k.objects().Get<Lv2Mutex>(id)) {
        const std::string d = mx->Describe();
        lv2 = d.empty() ? "lv2 mutex idle (free, no waiters)" : d;
    }
    std::snprintf(line, sizeof(line), "lwmutex 0x%08x (guest struct): owner 0x%08x, waiter field %u, queue id 0x%08x -> %s", ea, owner,
                  waiter, id, lv2.c_str());
    return line;
}

// Who can push into queue `id`: ports, timers, SPU threads/groups connected to it.
std::vector<std::string> QueueProducers(Kernel& k, u32 id) {
    std::vector<std::string> out;
    char line[160];
    for (const auto& obj : k.objects().Snapshot()) {
        if (const auto port = std::dynamic_pointer_cast<Lv2EventPort>(obj)) {
            std::unique_lock lock(port->m, std::try_to_lock);
            const auto q = lock ? port->queue.lock() : nullptr;
            if (q && q->id == id) {
                std::snprintf(line, sizeof(line), "producer: event port 0x%08x (name 0x%016" PRIx64 ")", port->id, port->port_name);
                out.emplace_back(line);
            }
        } else if (const auto timer = std::dynamic_pointer_cast<Lv2Timer>(obj)) {
            std::unique_lock lock(timer->m, std::try_to_lock);
            if (lock && timer->connected && timer->queue_id == id) {
                lock.unlock();
                out.push_back("producer: " + timer->Describe());
            }
        } else if (const auto spu = std::dynamic_pointer_cast<spu::SpuThread>(obj)) {
            std::unique_lock lock(spu->m, std::try_to_lock);
            if (!lock) continue;
            for (std::size_t i = 0; i < spu->ports.size(); ++i)
                if (const auto q = spu->ports[i].lock(); q && q->id == id) {
                    std::snprintf(line, sizeof(line), "producer: SPU thread 0x%08x \"%s\" port %zu", spu->id, spu->name.c_str(), i);
                    out.emplace_back(line);
                }
        } else if (const auto group = std::dynamic_pointer_cast<spu::SpuThreadGroup>(obj)) {
            std::unique_lock lock(group->m, std::try_to_lock);
            if (!lock) continue;
            static constexpr const char* kEvents[] = {"RUN", "EXCEPTION", "SYSTEM_MODULE"};
            for (std::size_t i = 0; i < group->event_queues.size(); ++i)
                if (const auto q = group->event_queues[i].lock(); q && q->id == id) {
                    std::snprintf(line, sizeof(line), "producer: SPU group 0x%08x \"%s\" %s events", group->id, group->name.c_str(), kEvents[i]);
                    out.emplace_back(line);
                }
        }
    }
    if (out.empty()) out.emplace_back("producer: no lv2 port, timer or SPU connection targets this queue (HLE producers such as cellAudio are not listed)");
    return out;
}

// What the blocked call waits on, decoded from its arguments (guest pointers validated).
std::vector<std::string> DecodeWait(Kernel& k, const ThreadManager::ThreadInfo& t, const std::vector<ThreadManager::ThreadInfo>& all) {
    std::vector<std::string> out;
    const u32 a0 = static_cast<u32>(t.live_args[0]);
    if (t.live_kind == ppu::PpuLiveStatus::kInSyscall) {
        switch (t.live_call) {
        case 130:
            if (const auto q = k.objects().Get<Lv2EventQueue>(a0)) {
                out.push_back(q->Describe() + "; " + Timeout(t.live_args[2]));
                for (auto& p : QueueProducers(k, a0)) out.push_back(std::move(p));
            } else {
                out.emplace_back("no event queue with that id");
            }
            break;
        case 107:
            if (const auto c = k.objects().Get<Lv2Cond>(a0)) {
                out.push_back(c->Stats() + "; " + Timeout(t.live_args[1]));
                AppendSignalers(k, *c, all, out);
            }
            break;
        case 102:
            if (const auto mx = k.objects().Get<Lv2Mutex>(a0)) out.push_back(mx->Describe());
            break;
        case 92:
            if (const auto sem = k.objects().Get<Lv2Semaphore>(a0)) out.push_back(sem->Describe());
            break;
        case 85:
            if (const auto f = k.objects().Get<Lv2EventFlag>(a0)) out.push_back(f->Describe());
            break;
        default: break;
        }
        return out;
    }
    if (t.live_kind != ppu::PpuLiveStatus::kInHle || t.live_call >= k.hle().size()) return out;
    const std::string& name = k.hle().At(t.live_call).name;
    vm::GuestMemory& m = k.memory();
    if (name == "sys_lwcond_wait") {
        char line[192];
        if (!a0 || !m.Check(a0, 8, vm::kPageReadable)) {
            std::snprintf(line, sizeof(line), "lwcond 0x%08x: not readable guest memory", a0);
            out.emplace_back(line);
            return out;
        }
        const u32 lwmutex = m.Read<u32>(a0), cond_id = m.Read<u32>(a0 + 4);
        const char* where = vm::GuestMemory::AreaOf(a0) == vm::Area::Stack ? " (on a thread stack: a local object)" : "";
        std::snprintf(line, sizeof(line), "lwcond 0x%08x (guest struct)%s: lwmutex 0x%08x, cond id 0x%08x; %s", a0, where, lwmutex, cond_id,
                      Timeout(t.live_args[1]).c_str());
        out.emplace_back(line);
        if (const auto c = k.objects().Get<Lv2Cond>(cond_id)) {
            out.push_back(c->Stats());
            AppendSignalers(k, *c, all, out);
        } else {
            out.emplace_back("no lv2 cond with that id (destroyed or never created)");
        }
        out.push_back(LwMutexLine(k, lwmutex));
    } else if (name == "sys_lwmutex_lock") {
        out.push_back(LwMutexLine(k, a0) + "; " + Timeout(t.live_args[1]));
    }
    return out;
}

// A few guest instructions before the call site (the code that set up the arguments).
std::vector<std::string> CodeBefore(const vm::GuestMemory& m, u32 lr, int count) {
    std::vector<std::string> out;
    if (lr < 4u * static_cast<u32>(count) || (lr & 3)) return out;
    for (u32 at = lr - 4u * static_cast<u32>(count); at <= lr; at += 4) {
        if (!m.Check(at, 4, vm::kPageReadable)) continue;
        char head[24];
        std::snprintf(head, sizeof(head), "%s0x%08x: ", at == lr - 4 ? "-> " : "   ", at);
        out.push_back(head + ppu::Disassemble(at, m.Read<u32>(at)));
    }
    return out;
}

}  // namespace

std::string StallMonitor::BuildReport(const std::string& reason) const {
    std::string out;
    char line[384];
    std::snprintf(line, sizeof(line), "\n== stall report: %s ==\n", reason.c_str());
    out += line;

    out += "PPU threads:\n";
    const auto threads = kernel_.threads().Snapshot();
    for (const auto& t : threads) {
        if (t.state == PpuThread::State::Exited) {
            std::snprintf(line, sizeof(line), "  0x%08x %-26s exited (0x%" PRIx64 ")\n", t.id, t.name.c_str(), t.exit_value);
            out += line;
            continue;
        }
        if (t.state == PpuThread::State::Created) {
            std::snprintf(line, sizeof(line), "  0x%08x %-26s created, not started\n", t.id, t.name.c_str());
            out += line;
            continue;
        }
        if (t.live_kind == ppu::PpuLiveStatus::kInPageFault) {
            std::snprintf(line, sizeof(line),
                          "  0x%08x %-26s SUSPENDED on a page fault: %s 0x%08" PRIx64 " at pc 0x%08x — waiting for the guest pager "
                          "(sys_ppu_thread_recover_page_fault)%s\n",
                          t.id, t.name.c_str(), t.live_args[1] ? "write" : "read", t.live_args[0], t.live_pc,
                          t.primary ? " [primary]" : "");
        } else if (t.live_kind == ppu::PpuLiveStatus::kRunning) {
            std::snprintf(line, sizeof(line), "  0x%08x %-26s RUNNING guest code near pc 0x%08x (%" PRIu64 " insns, %" PRIu64 " calls)%s\n",
                          t.id, t.name.c_str(), t.live_pc, t.live_instructions, t.live_calls, t.primary ? " [primary]" : "");
        } else {
            std::string args;
            const int arity = Arity(kernel_, t.live_kind, t.live_call);
            for (int i = 0; i < arity; ++i) {
                char a[24];
                std::snprintf(a, sizeof(a), "%s0x%" PRIx64, i ? ", " : "", t.live_args[i]);
                args += a;
            }
            std::snprintf(line, sizeof(line),
                          "  0x%08x %-26s BLOCKED in %s(%s)\n"
                          "             called from lr 0x%08x (thunk/sc at 0x%08x), %" PRIu64 " insns, %" PRIu64 " calls%s\n",
                          t.id, t.name.c_str(), CallName(kernel_, t.live_kind, t.live_call).c_str(), args.c_str(), t.live_lr, t.live_pc,
                          t.live_instructions, t.live_calls, t.primary ? " [primary]" : "");
            out += line;
            const std::string site = hle::DescribeCallSite(kernel_.memory(), t.live_lr);
            if (!site.empty()) out += "             caller: " + site + "\n";
            for (const std::string& d : DecodeWait(kernel_, t, threads)) out += "             " + d + "\n";
            if (t.primary) {
                for (const std::string& c : CodeBefore(kernel_.memory(), t.live_lr, 8)) out += "               " + c + "\n";
                if (!t.live_recent.empty()) out += "             completed calls before this one (oldest first):\n";
                for (const ppu::CallRecord& r : t.live_recent) out += "               " + RecentCallText(kernel_, r) + "\n";
            } else if (!t.live_recent.empty()) {
                // Compact: the last two calls this thread completed.
                std::string recent;
                for (std::size_t i = t.live_recent.size() > 2 ? t.live_recent.size() - 2 : 0; i < t.live_recent.size(); ++i)
                    recent += (recent.empty() ? "" : "; ") + RecentCallText(kernel_, t.live_recent[i]);
                out += "             before: " + recent + "\n";
            }
            continue;
        }
        out += line;
    }

    const auto spu = kernel_.spu().Describe();
    out += "SPU thread groups:\n";
    if (spu.empty()) out += "  (none)\n";
    for (const std::string& l : spu) out += "  " + l + "\n";

    out += "lv2 objects with waiters/owners (and timers):\n";
    std::size_t listed = 0;
    for (const auto& obj : kernel_.objects().Snapshot()) {
        const std::string d = obj->Describe();
        if (d.empty()) continue;
        out += "  " + d + "\n";
        ++listed;
    }
    if (!listed) out += "  (none)\n";

    for (const auto& [title, section] : sections_) {
        out += title + ":\n";
        for (const std::string& l : section()) out += "  " + l + "\n";
    }
    out += "== end of stall report ==\n";
    return out;
}

std::optional<u64> StallMonitor::PrimaryProgress() const {
    for (const auto& t : kernel_.threads().Snapshot())
        if (t.primary && t.state == PpuThread::State::Running) return t.live_instructions + t.live_calls;
    return std::nullopt;
}

void StallMonitor::Main() {
    host::SetCurrentThreadName("stall monitor");
    using Clock = std::chrono::steady_clock;
    u64 last = Progress();
    auto last_change = Clock::now();
    auto last_periodic = Clock::now();
    bool reported = false;  // once per stall episode
    // The global counter keeps moving while any thread runs — a paced background thread (an audio
    // thread woken every 5.33 ms, a SPURS handler restarting its group) hides a stuck main loop.
    // So the primary thread is also watched on its own.
    std::optional<u64> last_primary = PrimaryProgress();
    auto primary_change = Clock::now();
    bool primary_reported = false;
    std::unique_lock lock(mutex_);
    while (!stop_) {
        cv_.wait_for(lock, std::chrono::milliseconds(250), [&] { return stop_; });
        if (stop_) break;
        lock.unlock();
        const u64 now_progress = Progress();
        const std::optional<u64> primary = PrimaryProgress();
        const auto now = Clock::now();
        if (now_progress != last) {
            last = now_progress;
            last_change = now;
            reported = false;
        }
        if (primary != last_primary) {
            last_primary = primary;
            primary_change = now;
            primary_reported = false;
        }
        std::string report;
        if (stall_seconds_ && !reported && !primary_reported && primary && now - primary_change >= std::chrono::seconds(stall_seconds_) &&
            now - last_change < std::chrono::seconds(stall_seconds_) && !kernel_.is_terminating()) {
            primary_reported = true;
            report = BuildReport("primary thread made no progress for " + std::to_string(stall_seconds_) +
                                 " s while other threads keep running");
        } else if (stall_seconds_ && !reported && now - last_change >= std::chrono::seconds(stall_seconds_)) {
            reported = true;
            // Also covers shutdown: a thread that ignores termination keeps Shutdown() from
            // joining it, and the process would hang on exit without saying why.
            report = BuildReport(kernel_.is_terminating()
                                     ? "process is ending, but threads have not stopped for " + std::to_string(stall_seconds_) + " s"
                                     : "no PPU progress for " + std::to_string(stall_seconds_) +
                                           " s (every PPU thread is blocked or gone)");
        } else if (periodic_seconds_ && now - last_periodic >= std::chrono::seconds(periodic_seconds_)) {
            last_periodic = now;
            report = BuildReport("periodic status (--status-every)");
        }
        if (!report.empty()) {
            std::fwrite(report.data(), 1, report.size(), stdout);
            std::fflush(stdout);
            ++reports_;
        }
        lock.lock();
    }
}

}  // namespace cell::lv2
