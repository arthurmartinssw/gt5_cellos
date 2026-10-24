// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_spu.cpp
#include "lv2/sys_spu.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include "common/log.h"
#include "lv2/sys_event.h"

namespace cell::lv2 {

using spu::SpuThread;
using spu::SpuThreadGroup;
using State = SpuThreadGroup::State;

namespace {

std::string MakeLogName(u32 id, const std::string& name) {
    char text[48];
    std::snprintf(text, sizeof(text), "0x%08x ", id);
    return text + (name.empty() ? std::string("SPU") : name);
}

}  // namespace

SpuManager::~SpuManager() { Shutdown(); }

s32 SpuManager::Initialize(u32 max_usable_spu, u32 max_raw_spu) {
    // lv2 (as reimplemented by RPCS3) rejects only an impossible raw SPU count; a larger
    // max_usable_spu is accepted and simply capped by what the hardware gives the process.
    if (max_raw_spu > kPs3MaxRawSpus) return CELL_EINVAL;
    std::lock_guard lock(mutex_);
    limits_.initialized = true;
    limits_.max_usable_spu = max_usable_spu;
    limits_.max_raw_spu = max_raw_spu;
    LOG_INFO("lv2", "sys_spu_initialize(max_usable_spu=%u, max_raw_spu=%u)", max_usable_spu, max_raw_spu);
    if (max_usable_spu > kPs3UsableSpus)
        LOG_WARN("lv2", "game asks for %u usable SPUs; the PS3 provides %u", max_usable_spu, kPs3UsableSpus);
    return CELL_OK;
}

SpuManager::Limits SpuManager::limits() const {
    std::lock_guard lock(mutex_);
    return limits_;
}

void SpuManager::SetExecutorFactory(SpuExecutorFactory factory) {
    std::lock_guard lock(mutex_);
    factory_ = std::move(factory);
}

std::unique_ptr<spu::ISpuExecutor> SpuManager::CreateExecutor(SpuThread& thread) const {
    SpuExecutorFactory factory;
    {
        std::lock_guard lock(mutex_);
        factory = factory_;
    }
    return factory ? factory(thread) : nullptr;
}

std::shared_ptr<SpuThread> SpuManager::FindThread(u32 id) const { return kernel_.objects().Get<SpuThread>(id); }
std::shared_ptr<SpuThreadGroup> SpuManager::FindGroup(u32 id) const { return kernel_.objects().Get<SpuThreadGroup>(id); }

// ---------------------------------------------------------------------------------------
// Groups
// ---------------------------------------------------------------------------------------

s32 SpuManager::CreateGroup(const GroupParams& params, u32* out_id) {
    if (params.num == 0 || params.num > kPs3UsableSpus) return CELL_EINVAL;
    if (!(params.type & SYS_SPU_THREAD_GROUP_TYPE_COOPERATE_WITH_SYSTEM) &&
        (params.priority < 16 || params.priority > 255))
        return CELL_EINVAL;
    auto group = std::make_shared<SpuThreadGroup>();
    group->kernel = &kernel_;
    group->name = params.name;
    group->num = params.num;
    group->priority = params.priority;
    group->type = params.type;
    group->threads.resize(params.num);
    {
        std::lock_guard lock(mutex_);
        const u32 limit = std::min(limits_.max_usable_spu, kPs3UsableSpus);
        if (spus_in_use_ + params.num > limit) return CELL_EBUSY;
        spus_in_use_ += params.num;
        groups_.push_back(group);
    }
    *out_id = kernel_.objects().Add(kIdSpuThreadGroup, group);
    LOG_INFO("lv2", "sys_spu_thread_group_create(\"%s\", num=%u, prio=%d, type=0x%x) -> 0x%08x", params.name.c_str(),
             params.num, params.priority, static_cast<u32>(params.type), *out_id);
    return CELL_OK;
}

s32 SpuManager::DestroyGroup(u32 id) {
    const auto group = FindGroup(id);
    if (!group) return CELL_ESRCH;
    std::vector<std::shared_ptr<SpuThread>> threads;
    {
        std::lock_guard lock(group->m);
        if (group->state == State::Running || group->joining) return CELL_EBUSY;
        threads = group->threads;
        group->state = State::NotInitialized;
    }
    group->JoinHosts();
    for (const auto& t : threads) {
        if (!t) continue;
        kernel_.objects().Remove(t->id);
        kernel_.memory().Free(vm::Area::SpuThread, t->ctx.ls_ea);
    }
    kernel_.objects().Remove(id);
    std::lock_guard lock(mutex_);
    spus_in_use_ -= group->num;
    // Kept in groups_ until here so Describe() and Shutdown() see every live group.
    std::erase(groups_, group);
    return CELL_OK;
}

s32 SpuManager::InitializeThread(const ThreadParams& params, u32* out_id) {
    const auto group = FindGroup(params.group);
    if (!group) return CELL_ESRCH;
    if (params.spu_num >= group->num) return CELL_EINVAL;
    {
        std::lock_guard lock(group->m);
        if (group->state == State::Running || group->state == State::Stopped) return CELL_EBUSY;
        if (group->threads[params.spu_num]) return CELL_EBUSY;
    }

    // Local store: first 256 KiB of a 1 MiB window in the spu_thread area; the remaining
    // 768 KiB stay inaccessible so a stray EA past the LS faults instead of aliasing.
    vm::GuestMemory& memory = kernel_.memory();
    const auto ls = memory.Allocate(vm::Area::SpuThread, spu::kLsWindow, spu::kLsWindow, vm::kProtReadWrite);
    if (!ls) return CELL_ENOMEM;
    memory.Protect(*ls + spu::kLsSize, spu::kLsWindow - spu::kLsSize, vm::kProtNone);

    auto thread = std::make_shared<SpuThread>(kernel_, *group, params.spu_num);
    thread->name = params.name;
    thread->image = params.image;
    std::copy(std::begin(params.args), std::end(params.args), thread->args);
    thread->option = params.option;
    thread->ctx.ls_ea = *ls;
    thread->ctx.ls = memory.shadow() + *ls;
    const u32 id = kernel_.objects().Add(kIdSpuThread, thread);
    thread->log_name = MakeLogName(id, params.name);

    bool complete = false;
    {
        std::lock_guard lock(group->m);
        if (group->threads[params.spu_num] || group->state == State::Running || group->state == State::Stopped) {
            kernel_.objects().Remove(id);  // lost a race with another initialize/start
            memory.Free(vm::Area::SpuThread, *ls);
            return CELL_EBUSY;
        }
        group->threads[params.spu_num] = thread;
        complete = std::all_of(group->threads.begin(), group->threads.end(), [](const auto& t) { return t != nullptr; });
        if (complete) group->state = State::Initialized;
    }
    *out_id = id;
    LOG_INFO("lv2",
             "sys_spu_thread_initialize(\"%s\", group 0x%08x, spu %u, entry 0x%05x, %zu segments) -> 0x%08x, LS at "
             "EA 0x%08x%s",
             params.name.c_str(), params.group, params.spu_num, params.image.entry, params.image.segments.size(), id,
             *ls, complete ? " (group complete)" : "");
    return CELL_OK;
}

s32 SpuManager::StartGroup(u32 id) {
    const auto group = FindGroup(id);
    if (!group) return CELL_ESRCH;
    std::shared_ptr<Lv2EventQueue> run_queue;
    {
        std::lock_guard lock(group->m);
        // Initialized implies no host threads: JoinGroup takes them in the same critical section
        // that moves the group back to Initialized.
        if (group->state != State::Initialized) return CELL_ESTAT;
        for (const auto& t : group->threads) {
            std::string error;
            if (!t->ResetForStart(&error)) {
                LOG_ERROR("lv2", "sys_spu_thread_group_start(0x%08x): SPU thread 0x%08x image: %s", id, t->id, error.c_str());
                return CELL_EFAULT;
            }
        }
        group->state = State::Running;
        group->running = group->num;
        group->join_cause = 0;
        group->exit_status = 0;
        group->run_insns = 0;
        run_queue = group->event_queues[0].lock();
        group->stats.OnStart(ppu::PpuInterpreter::SystemTimeMicroseconds());
        // Bounded logging (spu/spu_run_stats.h): the first runs of each group, then a periodic
        // summary; --log-spu-runs logs every run. The run itself is never affected.
        const spu::SpuRunLogConfig& cfg = spu::RunLogConfig();
        const u32 first = cfg.first_runs.load(std::memory_order_relaxed);
        const bool log_run = cfg.verbose.load(std::memory_order_relaxed) || group->stats.starts <= first;
        group->log_this_run.store(log_run, std::memory_order_relaxed);
        if (log_run)
            LOG_INFO("lv2", "sys_spu_thread_group_start(0x%08x \"%s\"): %u SPU thread(s) on host threads", id,
                     group->name.c_str(), group->num);
        else if (group->stats.starts == u64{first} + 1)
            LOG_INFO("lv2", "SPU group 0x%08x \"%s\" restarted again (run %llu): further start/exit messages are counted, "
                     "summary every %u s (--log-spu-runs logs every run)",
                     id, group->name.c_str(), static_cast<unsigned long long>(group->stats.starts),
                     cfg.summary_seconds.load(std::memory_order_relaxed));
        // Each SPU thread becomes a host thread. A thread that stops at once blocks in
        // OnThreadStopped() on group->m until this block ends: no lost transition.
        for (const auto& t : group->threads) group->hosts.emplace_back([t] { t->HostMain(); });
    }
    // SYS_SPU_THREAD_GROUP_EVENT_RUN, sent outside the group lock (queue locks are never nested in it).
    if (run_queue) run_queue->Push({spu::SYS_SPU_THREAD_GROUP_EVENT_RUN_KEY, id, 0, 0});
    return CELL_OK;
}

s32 SpuManager::TerminateGroup(u32 id, s32 value) {
    const auto group = FindGroup(id);
    if (!group) return CELL_ESRCH;
    {
        std::lock_guard lock(group->m);
        if (group->state != State::Running) return CELL_ESTAT;
    }
    group->RequestStop(spu::SYS_SPU_THREAD_GROUP_JOIN_TERMINATED, value);
    return CELL_OK;
}

s32 SpuManager::JoinGroup(u32 id, s32* cause, s32* status) {
    const auto group = FindGroup(id);
    if (!group) return CELL_ESRCH;
    std::vector<std::thread> finished;
    {
        std::unique_lock lock(group->m);
        if (group->state != State::Running && group->state != State::Stopped) return CELL_ESTAT;
        if (group->joining) return CELL_EBUSY;
        group->joining = true;
        const auto r = kernel_.Wait(lock, group->cv, 0, [&] { return group->state == State::Stopped; });
        group->joining = false;
        if (r == WaitResult::Terminated) return CELL_ECANCELED;
        *cause = group->join_cause;
        *status = group->exit_status;
        group->state = State::Initialized;  // may be started again
        finished.swap(group->hosts);        // this run's host threads, all past OnThreadStopped
    }
    for (auto& h : finished) h.join();
    return CELL_OK;
}

void SpuManager::Shutdown() {
    std::vector<std::shared_ptr<SpuThreadGroup>> groups;
    {
        std::lock_guard lock(mutex_);
        groups = groups_;
    }
    for (const auto& g : groups) {
        g->RequestStop(spu::SYS_SPU_THREAD_GROUP_JOIN_TERMINATED, 0);
        g->JoinHosts();
    }
}

// What an SPU thread is inside of (SpuThread::live_wait), for the stall report.
static std::string DescribeSpuWait(s32 wait) {
    if (wait < 0) return "running";
    if (wait == spu::SpuThread::kWaitPageFault) return "SUSPENDED on a page fault (waiting for sys_spu_thread_recover_page_fault)";
    char text[64];
    if (wait & spu::SpuThread::kWaitStop) {
        const u32 code = static_cast<u32>(wait) & 0x3FFF;
        const char* what = code == 0x110 ? "sys_spu_thread_receive_event" : code == 0x111 ? "sys_spu_thread_tryreceive_event"
                         : code == 0x100 ? "yield" : "service";
        std::snprintf(text, sizeof(text), "in stop 0x%03x (%s)", code, what);
        return text;
    }
    const char* name = nullptr;
    switch (wait) {
    case 0: name = "SPU_RdEventStat"; break;
    case 3: name = "SPU_RdSigNotify1"; break;
    case 4: name = "SPU_RdSigNotify2"; break;
    case 28: name = "SPU_WrOutMbox"; break;
    case 29: name = "SPU_RdInMbox"; break;
    case 30: name = "SPU_WrOutIntrMbox"; break;
    default: break;
    }
    if (name) std::snprintf(text, sizeof(text), "in channel %d (%s)", wait, name);
    else std::snprintf(text, sizeof(text), "in channel %d", wait);
    return text;
}

std::vector<std::string> SpuManager::Describe() const {
    static constexpr const char* kStates[] = {"not initialized", "initialized", "running", "stopped"};
    static constexpr const char* kExits[] = {"-", "thread exit", "group exit", "terminated", "stop code", "fault"};
    std::vector<std::shared_ptr<SpuThreadGroup>> groups;
    {
        std::lock_guard lock(mutex_);
        groups = groups_;
    }
    std::vector<std::string> out;
    char line[256];
    for (const auto& g : groups) {
        std::lock_guard lock(g->m);
        std::snprintf(line, sizeof(line), "group 0x%08x \"%s\": %u SPU(s), %s, join cause 0x%x, status %d", g->id,
                      g->name.c_str(), g->num, kStates[static_cast<int>(g->state)], static_cast<u32>(g->join_cause),
                      g->exit_status);
        out.emplace_back(line);
        if (g->stats.starts) out.back() += "; runs: " + g->stats.Totals();
        for (const auto& t : g->threads) {
            if (!t) continue;
            std::lock_guard tl(t->m);
            std::snprintf(line, sizeof(line), "  spu %u thread 0x%08x \"%s\": LS 0x%08x, entry 0x%05x, exit %s 0x%x%s%s",
                          t->spu_num, t->id, t->name.c_str(), t->ctx.ls_ea, t->image.entry,
                          kExits[static_cast<int>(t->exit)], t->exit_status, t->fault.empty() ? "" : " — ",
                          t->fault.c_str());
            out.emplace_back(line);
            if (g->state == SpuThreadGroup::State::Running && t->exit == spu::SpuExit::None) {
                std::snprintf(line, sizeof(line), "      pc 0x%05x, %" PRIu64 " instructions, %s, in_mbox %zu, out_mbox %s, event mask 0x%x stat 0x%x",
                              t->live_pc.load(std::memory_order_relaxed), t->live_instructions.load(std::memory_order_relaxed),
                              DescribeSpuWait(t->live_wait.load(std::memory_order_relaxed)).c_str(), t->in_mbox.size(),
                              t->out_mbox ? "full" : "empty", t->event_mask, t->event_stat);
                out.emplace_back(line);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// Syscalls
// ---------------------------------------------------------------------------------------

namespace {

using C = ppu::PpuThreadContext;
SpuManager& S(void* p) { return *static_cast<SpuManager*>(p); }
u32 A32(const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); }

std::string ReadName(const vm::GuestMemory& m, u32 addr, u32 max) {
    if (!addr) return {};
    return m.ReadCString(addr, std::max<u32>(max, 1) + 1).value_or("");
}

// Lookup helpers that also hand back the thread's group lock state.
bool GroupRunning(SpuThreadGroup& g) {
    std::lock_guard lock(g.m);
    return g.state == State::Running;
}

}  // namespace

void SpuManager::RegisterSyscalls(SyscallTable& table) {
    // A handler needs the kernel (memory, objects) besides the manager: reach it via kernel_.
    table.Register(
        SYS_SPU_INITIALIZE, "sys_spu_initialize",
        [](void* p, C& c) -> s32 { return S(p).Initialize(A32(c, 0), A32(c, 1)); }, this);

    // 170 (u32* id, u32 num, s32 prio, sys_spu_thread_group_attribute* attr)
    //   attr: be32 nsize; be32 name; be32 type; be32 ct
    table.Register(
        170, "sys_spu_thread_group_create",
        [](void* p, C& c) -> s32 {
            SpuManager& s = S(p);
            vm::GuestMemory& m = s.kernel_.memory();
            const u32 out = A32(c, 0), attr = A32(c, 3);
            if (!m.Check(out, 4, vm::kPageWritable) || !m.Check(attr, 16, vm::kPageReadable)) return CELL_EFAULT;
            GroupParams g;
            g.num = A32(c, 1);
            g.priority = static_cast<s32>(c.gpr[5]);
            g.type = static_cast<s32>(m.Read<u32>(attr + 8));
            g.name = ReadName(m, m.Read<u32>(attr + 4), m.Read<u32>(attr));
            u32 id = 0;
            const s32 r = s.CreateGroup(g, &id);
            if (r == CELL_OK) m.Write<u32>(out, id);
            return r;
        },
        this);

    table.Register(
        171, "sys_spu_thread_group_destroy", [](void* p, C& c) -> s32 { return S(p).DestroyGroup(A32(c, 0)); }, this);

    // 172 (u32* thread, u32 group, u32 spu_num, sys_spu_image_t* img, sys_spu_thread_attribute* attr,
    //      sys_spu_thread_argument* arg)
    //   attr: be32 name; be32 name_len; be32 option        arg: be64 arg1..arg4
    table.Register(
        172, "sys_spu_thread_initialize",
        [](void* p, C& c) -> s32 {
            SpuManager& s = S(p);
            vm::GuestMemory& m = s.kernel_.memory();
            const u32 out = A32(c, 0), img = A32(c, 3), attr = A32(c, 4), arg = A32(c, 5);
            if (!m.Check(out, 4, vm::kPageWritable) || !m.Check(attr, 12, vm::kPageReadable) ||
                !m.Check(arg, 32, vm::kPageReadable))
                return CELL_EFAULT;
            ThreadParams t;
            t.group = A32(c, 1);
            t.spu_num = A32(c, 2);
            std::string error;
            const auto image = spu::ReadSpuImage(m, img, &error);
            if (!image) {
                LOG_ERROR("lv2", "sys_spu_thread_initialize: bad sys_spu_image_t at 0x%08x: %s", img, error.c_str());
                return CELL_EINVAL;
            }
            t.image = *image;
            t.name = ReadName(m, m.Read<u32>(attr), m.Read<u32>(attr + 4));
            t.option = m.Read<u32>(attr + 8);
            for (int i = 0; i < 4; ++i) t.args[i] = m.Read<u64>(arg + 8 * i);
            u32 id = 0;
            const s32 r = s.InitializeThread(t, &id);
            if (r == CELL_OK) m.Write<u32>(out, id);
            return r;
        },
        this);

    table.Register(
        173, "sys_spu_thread_group_start", [](void* p, C& c) -> s32 { return S(p).StartGroup(A32(c, 0)); }, this);

    table.Register(
        176, "sys_spu_thread_group_yield",
        [](void* p, C& c) -> s32 { return S(p).FindGroup(A32(c, 0)) ? CELL_OK : CELL_ESRCH; }, this);

    table.Register(
        177, "sys_spu_thread_group_terminate",
        [](void* p, C& c) -> s32 { return S(p).TerminateGroup(A32(c, 0), static_cast<s32>(c.gpr[4])); }, this);

    // 178 (u32 id, u32* cause, u32* status): both pointers optional
    table.Register(
        178, "sys_spu_thread_group_join",
        [](void* p, C& c) -> s32 {
            SpuManager& s = S(p);
            vm::GuestMemory& m = s.kernel_.memory();
            const u32 cause_ptr = A32(c, 1), status_ptr = A32(c, 2);
            if ((cause_ptr && !m.Check(cause_ptr, 4, vm::kPageWritable)) ||
                (status_ptr && !m.Check(status_ptr, 4, vm::kPageWritable)))
                return CELL_EFAULT;
            s32 cause = 0, status = 0;
            const s32 r = s.JoinGroup(A32(c, 0), &cause, &status);
            if (r != CELL_OK) return r;
            if (cause_ptr) m.Write<u32>(cause_ptr, static_cast<u32>(cause));
            if (status_ptr) m.Write<u32>(status_ptr, static_cast<u32>(status));
            return CELL_OK;
        },
        this);

    table.Register(
        179, "sys_spu_thread_group_set_priority",
        [](void* p, C& c) -> s32 {
            const auto g = S(p).FindGroup(A32(c, 0));
            if (!g) return CELL_ESRCH;
            const s32 prio = static_cast<s32>(c.gpr[4]);
            if (prio < 16 || prio > 255) return CELL_EINVAL;
            std::lock_guard lock(g->m);
            g->priority = prio;  // recorded; the host scheduler decides
            return CELL_OK;
        },
        this);

    table.Register(
        180, "sys_spu_thread_group_get_priority",
        [](void* p, C& c) -> s32 {
            SpuManager& s = S(p);
            vm::GuestMemory& m = s.kernel_.memory();
            const auto g = s.FindGroup(A32(c, 0));
            if (!g) return CELL_ESRCH;
            if (!m.Check(A32(c, 1), 4, vm::kPageWritable)) return CELL_EFAULT;
            std::lock_guard lock(g->m);
            m.Write<u32>(A32(c, 1), static_cast<u32>(g->priority));
            return CELL_OK;
        },
        this);

    // 165 (u32 id, s32* status)
    table.Register(
        165, "sys_spu_thread_get_exit_status",
        [](void* p, C& c) -> s32 {
            SpuManager& s = S(p);
            vm::GuestMemory& m = s.kernel_.memory();
            const auto t = s.FindThread(A32(c, 0));
            if (!t) return CELL_ESRCH;
            if (!m.Check(A32(c, 1), 4, vm::kPageWritable)) return CELL_EFAULT;
            std::lock_guard lock(t->m);
            if (t->exit != spu::SpuExit::ThreadExit) return CELL_ESTAT;
            m.Write<u32>(A32(c, 1), t->exit_status);
            return CELL_OK;
        },
        this);

    // 166 (u32 id, sys_spu_thread_argument* arg)
    table.Register(
        166, "sys_spu_thread_set_argument",
        [](void* p, C& c) -> s32 {
            SpuManager& s = S(p);
            vm::GuestMemory& m = s.kernel_.memory();
            const auto t = s.FindThread(A32(c, 0));
            if (!t) return CELL_ESRCH;
            if (!m.Check(A32(c, 1), 32, vm::kPageReadable)) return CELL_EFAULT;
            std::lock_guard lock(t->m);
            for (int i = 0; i < 4; ++i) t->args[i] = m.Read<u64>(A32(c, 1) + 8 * i);
            return CELL_OK;
        },
        this);

    // 181 (u32 id, u32 lsa, u64 value, u32 type) / 182 (u32 id, u32 lsa, u64* value, u32 type)
    // The LS is live memory of a running SPU: accessed with relaxed atomics, big-endian.
    auto ls_access = [](void* p, C& c, bool write) -> s32 {
        SpuManager& s = S(p);
        vm::GuestMemory& m = s.kernel_.memory();
        const auto t = s.FindThread(A32(c, 0));
        if (!t) return CELL_ESRCH;
        const u32 lsa = A32(c, 1), type = A32(c, 3);
        if ((type != 1 && type != 2 && type != 4 && type != 8) || lsa % type || lsa + type > spu::kLsSize)
            return CELL_EINVAL;
        if (!GroupRunning(t->group)) return CELL_ESTAT;
        u8* at = t->ctx.ls + lsa;
        auto rw = [&]<typename T>(T*) -> u64 {
            std::atomic_ref<T> ref(*reinterpret_cast<T*>(at));
            if (write) ref.store(ByteSwap(static_cast<T>(c.gpr[5])), std::memory_order_relaxed);
            return ByteSwap(ref.load(std::memory_order_relaxed));
        };
        if (!write && !m.Check(A32(c, 2), 8, vm::kPageWritable)) return CELL_EFAULT;
        u64 value = 0;
        switch (type) {
        case 1: value = rw(static_cast<u8*>(nullptr)); break;
        case 2: value = rw(static_cast<u16*>(nullptr)); break;
        case 4: value = rw(static_cast<u32*>(nullptr)); break;
        default: value = rw(static_cast<u64*>(nullptr)); break;
        }
        if (!write) m.Write<u64>(A32(c, 2), value);
        return CELL_OK;
    };
    table.Register(
        181, "sys_spu_thread_write_ls", [](void* p, C& c) -> s32 { return decltype(ls_access){}(p, c, true); }, this);
    table.Register(
        182, "sys_spu_thread_read_ls", [](void* p, C& c) -> s32 { return decltype(ls_access){}(p, c, false); }, this);

    // 184 (u32 id, u32 number, u32 value)
    table.Register(
        184, "sys_spu_thread_write_snr",
        [](void* p, C& c) -> s32 {
            const auto t = S(p).FindThread(A32(c, 0));
            if (!t) return CELL_ESRCH;
            return t->WriteSignal(A32(c, 1), A32(c, 2));
        },
        this);

    // 185 (u32 id, u32 eq, u32 et) / 186 (u32 id, u32 et): group events (RUN, EXCEPTION,
    // SYSTEM_MODULE). Connected queues are recorded; nothing raises these events yet.
    table.Register(
        185, "sys_spu_thread_group_connect_event",
        [](void* p, C& c) -> s32 {
            SpuManager& s = S(p);
            const auto g = s.FindGroup(A32(c, 0));
            if (!g) return CELL_ESRCH;
            const auto q = s.kernel_.objects().Get<Lv2EventQueue>(A32(c, 1));
            if (!q) return CELL_ESRCH;
            const u32 et = A32(c, 2);
            const int slot = et == SYS_SPU_THREAD_GROUP_EVENT_RUN ? 0
                             : et == SYS_SPU_THREAD_GROUP_EVENT_EXCEPTION ? 1
                             : et == SYS_SPU_THREAD_GROUP_EVENT_SYSTEM_MODULE ? 2 : -1;
            if (slot < 0) return CELL_EINVAL;
            std::lock_guard lock(g->m);
            if (g->event_queues[slot].lock()) return CELL_EBUSY;
            g->event_queues[slot] = q;
            return CELL_OK;
        },
        this);
    table.Register(
        186, "sys_spu_thread_group_disconnect_event",
        [](void* p, C& c) -> s32 {
            const auto g = S(p).FindGroup(A32(c, 0));
            if (!g) return CELL_ESRCH;
            const u32 et = A32(c, 1);
            const int slot = et == SYS_SPU_THREAD_GROUP_EVENT_RUN ? 0
                             : et == SYS_SPU_THREAD_GROUP_EVENT_EXCEPTION ? 1
                             : et == SYS_SPU_THREAD_GROUP_EVENT_SYSTEM_MODULE ? 2 : -1;
            if (slot < 0) return CELL_EINVAL;
            std::lock_guard lock(g->m);
            if (!g->event_queues[slot].lock()) return CELL_ENOTCONN;
            g->event_queues[slot].reset();
            return CELL_OK;
        },
        this);

    // 250 (u32 id, u32 threads): cooperative-victim hint for the system scheduler; nothing to do here.
    table.Register(
        250, "sys_spu_thread_group_set_cooperative_victims",
        [](void* p, C& c) -> s32 { return S(p).FindGroup(A32(c, 0)) ? CELL_OK : CELL_ESRCH; }, this);

    // 251 (u32 id, u32 eq, u64 req, u8* spup): connect the first port set in `req` that is free on
    // every thread of the group to the queue (sys_spu_thread_send_event from any of them lands
    // there). cellSpursAttachLv2EventQueue goes through this.
    table.Register(
        251, "sys_spu_thread_group_connect_event_all_threads",
        [](void* p, C& c) -> s32 {
            SpuManager& s = S(p);
            vm::GuestMemory& m = s.kernel_.memory();
            const auto g = s.FindGroup(A32(c, 0));
            if (!g) return CELL_ESRCH;
            const auto q = s.kernel_.objects().Get<Lv2EventQueue>(A32(c, 1));
            if (!q) return CELL_ESRCH;
            const u64 req = c.gpr[5];
            const u32 out = A32(c, 3);
            if (!req || q->type != SYS_PPU_QUEUE) return CELL_EINVAL;
            if (!m.Check(out, 1, vm::kPageWritable)) return CELL_EFAULT;
            std::vector<std::shared_ptr<SpuThread>> threads;
            {
                std::lock_guard lock(g->m);
                if (g->state == State::NotInitialized) return CELL_ESTAT;
                threads = g->threads;
            }
            for (u32 port = 0; port < spu::kSpuPortCount; ++port) {
                if (!((req >> port) & 1)) continue;
                bool free = true;
                for (const auto& t : threads) {
                    std::lock_guard tl(t->m);
                    free &= !t->ports[port].lock();
                }
                if (!free) continue;
                for (const auto& t : threads) {
                    std::lock_guard tl(t->m);
                    t->ports[port] = q;
                }
                m.Write<u8>(out, static_cast<u8>(port));
                return CELL_OK;
            }
            return CELL_EISCONN;
        },
        this);
    // 252 (u32 id, u8 spup)
    table.Register(
        252, "sys_spu_thread_group_disconnect_event_all_threads",
        [](void* p, C& c) -> s32 {
            const auto g = S(p).FindGroup(A32(c, 0));
            if (!g) return CELL_ESRCH;
            const u32 port = A32(c, 1) & 0xFF;
            if (port >= spu::kSpuPortCount) return CELL_EINVAL;
            std::vector<std::shared_ptr<SpuThread>> threads;
            {
                std::lock_guard lock(g->m);
                threads = g->threads;
            }
            bool any = false;
            for (const auto& t : threads) {
                if (!t) continue;
                std::lock_guard tl(t->m);
                any |= static_cast<bool>(t->ports[port].lock());
                t->ports[port].reset();
            }
            return any ? CELL_OK : CELL_ENOTCONN;
        },
        this);

    // 187 (u32 id, u64 value): bit 0 / bit 1 = SNR1 / SNR2 in OR (accumulate) mode
    table.Register(
        187, "sys_spu_thread_set_spu_cfg",
        [](void* p, C& c) -> s32 {
            const auto t = S(p).FindThread(A32(c, 0));
            if (!t) return CELL_ESRCH;
            if (c.gpr[4] > 3) return CELL_EINVAL;
            std::lock_guard lock(t->m);
            t->snr_or_mode[0] = c.gpr[4] & 1;
            t->snr_or_mode[1] = c.gpr[4] & 2;
            return CELL_OK;
        },
        this);
    table.Register(
        188, "sys_spu_thread_get_spu_cfg",
        [](void* p, C& c) -> s32 {
            SpuManager& s = S(p);
            vm::GuestMemory& m = s.kernel_.memory();
            const auto t = s.FindThread(A32(c, 0));
            if (!t) return CELL_ESRCH;
            if (!m.Check(A32(c, 1), 8, vm::kPageWritable)) return CELL_EFAULT;
            std::lock_guard lock(t->m);
            m.Write<u64>(A32(c, 1), (t->snr_or_mode[0] ? 1u : 0u) | (t->snr_or_mode[1] ? 2u : 0u));
            return CELL_OK;
        },
        this);

    // 190 (u32 id, u32 value)
    table.Register(
        190, "sys_spu_thread_write_spu_mb",
        [](void* p, C& c) -> s32 {
            const auto t = S(p).FindThread(A32(c, 0));
            if (!t) return CELL_ESRCH;
            if (!GroupRunning(t->group)) return CELL_ESTAT;
            t->WriteInboundMailbox(A32(c, 1));
            return CELL_OK;
        },
        this);

    // 191 (u32 id, u32 eq, u32 et, u8 spup): SPU port -> PPU event queue (send/throw_event)
    table.Register(
        191, "sys_spu_thread_connect_event",
        [](void* p, C& c) -> s32 {
            SpuManager& s = S(p);
            const auto t = s.FindThread(A32(c, 0));
            if (!t) return CELL_ESRCH;
            const auto q = s.kernel_.objects().Get<Lv2EventQueue>(A32(c, 1));
            if (!q) return CELL_ESRCH;
            const u32 spup = A32(c, 3) & 0xFF;
            if (A32(c, 2) != SYS_SPU_THREAD_EVENT_USER || spup >= spu::kSpuPortCount || q->type != SYS_PPU_QUEUE)
                return CELL_EINVAL;
            std::lock_guard lock(t->m);
            if (t->ports[spup].lock()) return CELL_EISCONN;
            t->ports[spup] = q;
            return CELL_OK;
        },
        this);
    // 192 (u32 id, u32 et, u8 spup)
    table.Register(
        192, "sys_spu_thread_disconnect_event",
        [](void* p, C& c) -> s32 {
            const auto t = S(p).FindThread(A32(c, 0));
            if (!t) return CELL_ESRCH;
            const u32 spup = A32(c, 2) & 0xFF;
            if (A32(c, 1) != SYS_SPU_THREAD_EVENT_USER || spup >= spu::kSpuPortCount) return CELL_EINVAL;
            std::lock_guard lock(t->m);
            if (!t->ports[spup].lock()) return CELL_ENOTCONN;
            t->ports[spup].reset();
            return CELL_OK;
        },
        this);

    // 193 (u32 id, u32 spuq, u32 spuq_num): SPU-side receive queue (stop 0x110)
    table.Register(
        193, "sys_spu_thread_bind_queue",
        [](void* p, C& c) -> s32 {
            SpuManager& s = S(p);
            const auto t = s.FindThread(A32(c, 0));
            if (!t) return CELL_ESRCH;
            const auto q = s.kernel_.objects().Get<Lv2EventQueue>(A32(c, 1));
            if (!q) return CELL_ESRCH;
            if (q->type != SYS_SPU_QUEUE) return CELL_EINVAL;
            const u32 number = A32(c, 2);
            std::lock_guard lock(t->m);
            for (const auto& [n, weak] : t->bound_queues)
                if (n == number || weak.lock() == q) return CELL_EBUSY;
            if (t->bound_queues.size() >= kMaxBoundSpuQueues) return CELL_EAGAIN;
            t->bound_queues.emplace_back(number, q);
            return CELL_OK;
        },
        this);
    // 194 (u32 id, u32 spuq_num)
    table.Register(
        194, "sys_spu_thread_unbind_queue",
        [](void* p, C& c) -> s32 {
            const auto t = S(p).FindThread(A32(c, 0));
            if (!t) return CELL_ESRCH;
            std::lock_guard lock(t->m);
            const auto it = std::find_if(t->bound_queues.begin(), t->bound_queues.end(),
                                         [&](const auto& b) { return b.first == A32(c, 1); });
            if (it == t->bound_queues.end()) return CELL_ESRCH;
            t->bound_queues.erase(it);
            return CELL_OK;
        },
        this);
}

}  // namespace cell::lv2
