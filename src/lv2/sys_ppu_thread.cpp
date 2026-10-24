// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_ppu_thread.cpp
#include "lv2/sys_ppu_thread.h"

#include <cstdio>
#include <cstring>

#include "common/host_thread.h"
#include "common/log.h"
#include "lv2/sys_mmapper.h"

namespace cell::lv2 {

namespace {
constexpr u32 kStackGuard = vm::k64K;
constexpr u32 kMinStack = 0x4000;
constexpr s32 kMaxPriority = 3071;  // lv2: 0 (highest) .. 3071 (lowest)
constexpr u32 kFrameHeader = 0x70;  // PPC64 ABI minimum stack frame

std::string MakeLogName(u32 id, const std::string& name) {
    char text[64];
    std::snprintf(text, sizeof(text), "0x%08x %s", id, name.c_str());
    return text;
}
}  // namespace

ThreadManager::~ThreadManager() { Shutdown(); }

std::shared_ptr<PpuThread> ThreadManager::FindLocked(u32 id) const {
    const auto it = threads_.find(id);
    return it == threads_.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------------------
// Primary thread
// ---------------------------------------------------------------------------------------

void ThreadManager::AdoptPrimary(const ppu::PpuThreadContext& ctx, s32 priority, std::string name) {
    auto t = std::make_shared<PpuThread>();
    t->id = kIdPpuThread;
    t->name = std::move(name);
    t->log_name = MakeLogName(t->id, t->name);
    t->ctx = ctx;
    t->ctx.thread_id = t->id;
    t->priority = priority;
    t->stack_base = ctx.stack_addr;
    t->stack_size = ctx.stack_size;
    t->primary = true;
    t->state = PpuThread::State::Running;
    std::lock_guard lock(mutex_);
    threads_[t->id] = t;
    primary_ = t;
}

namespace {
// PPU access to a page that is not accessible: give demand paging (sys_mmapper) a chance.
bool PpuPageFault(void* kernel, ppu::PpuThreadContext& ctx, u32 ea, bool write) {
    return static_cast<Kernel*>(kernel)->mmapper().HandlePageFault(SYS_MEMORY_PAGE_FAULT_TYPE_PPU_THREAD, ctx.thread_id, ea, write);
}
}  // namespace

ppu::StopInfo ThreadManager::RunPrimary(u64 max_instructions, bool trace) {
    PpuThread& t = *primary_;
    log::tls_thread_name = t.log_name.c_str();
    Kernel::SetCurrentThreadId(t.id);
    ppu::PpuInterpreter cpu(kernel_.memory(), kernel_.hle(), kernel_.syscalls(), kernel_.code(), &kernel_.terminating());
    cpu.set_trace(trace);
    cpu.set_live_status(&t.live);
    cpu.set_page_fault_handler(PpuPageFault, &kernel_);
    const ppu::StopInfo stop = cpu.Execute(t.ctx, max_instructions);
    OnStopped(t, stop, cpu);
    log::tls_thread_name = nullptr;
    Kernel::SetCurrentThreadId(0);
    return stop;
}

const ppu::PpuThreadContext& ThreadManager::primary_context() const { return primary_->ctx; }

void ThreadManager::WaitForAllThreads() {
    std::unique_lock lock(mutex_);
    kernel_.Wait(lock, cv_, 0, [&] {
        for (const auto& [id, t] : threads_)
            if (t->state == PpuThread::State::Running) return false;
        return true;
    });
}

void ThreadManager::Shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (shut_down_) return;
        shut_down_ = true;  // Start() refuses from now on
    }
    if (!kernel_.is_terminating()) {
        ProcessEnd end;
        end.kind = ppu::kExitProcess;
        kernel_.RequestTermination(end);
    }
    std::vector<std::thread> joining;
    {
        std::lock_guard lock(mutex_);
        joining.swap(host_threads_);
    }
    for (std::thread& th : joining)  // threads take mutex_ in OnStopped: join without holding it
        if (th.joinable()) th.join();
}

void ThreadManager::WakeForTermination() {
    std::lock_guard lock(mutex_);
    cv_.notify_all();
}

// ---------------------------------------------------------------------------------------
// Thread body and exit handling
// ---------------------------------------------------------------------------------------

void ThreadManager::HostMain(std::shared_ptr<PpuThread> thread) {
    char host_name[32];
    std::snprintf(host_name, sizeof(host_name), "PPU 0x%08x", thread->id);
    host::SetCurrentThreadName(host_name);  // visible in debuggers / profilers / crash dumps
    log::tls_thread_name = thread->log_name.c_str();
    Kernel::SetCurrentThreadId(thread->id);
    LOG_TRACE("ppu", "thread started at pc 0x%08x", thread->ctx.cia);
    ppu::PpuInterpreter cpu(kernel_.memory(), kernel_.hle(), kernel_.syscalls(), kernel_.code(), &kernel_.terminating());
    cpu.set_live_status(&thread->live);
    cpu.set_page_fault_handler(PpuPageFault, &kernel_);
    const ppu::StopInfo stop = cpu.Execute(thread->ctx, ~u64{0});
    OnStopped(*thread, stop, cpu);
    log::tls_thread_name = nullptr;
}

void ThreadManager::OnStopped(PpuThread& thread, const ppu::StopInfo& stop, const ppu::PpuInterpreter& cpu) {
    // Decide under the lock, act on the kernel after releasing it (RequestTermination takes
    // this mutex again through WakeForTermination).
    bool terminate = false;
    ProcessEnd end;
    {
        std::lock_guard lock(mutex_);
        thread.stop = stop;
        thread.recent_pcs = cpu.RecentPcs();
        thread.recent_calls = cpu.RecentCalls();
        thread.exit_value = thread.ctx.exit_status;

        if (stop.reason == ppu::StopReason::Exited && thread.ctx.exit_kind == ppu::kExitThread) {
            LOG_TRACE("ppu", "thread exited, value 0x%llx", static_cast<unsigned long long>(thread.exit_value));
        } else if (stop.reason == ppu::StopReason::Terminated) {
            // Another thread ended the process.
        } else {
            terminate = true;
            end.thread_id = thread.id;
            end.kind = thread.ctx.exit_kind;
            end.status = thread.ctx.exit_status;
            end.fatal = stop.reason != ppu::StopReason::Exited || thread.ctx.exit_kind == ppu::kExitAbort;
            if (thread.ctx.exit_kind == ppu::kExitAbort && thread.ctx.exit_message) end.message = thread.ctx.exit_message;
            end.stop = stop;
            end.context = thread.ctx;
            end.recent_pcs = thread.recent_pcs;
            end.recent_calls = thread.recent_calls;
            if (end.fatal)
                LOG_ERROR("ppu", "thread stopped: %s at pc 0x%08x (instruction 0x%08x) %s", ppu::StopReasonName(stop.reason),
                          stop.pc, stop.instruction, stop.detail.c_str());
        }
        thread.state = PpuThread::State::Exited;
        if (!thread.joinable && !thread.primary) ReapLocked(thread);
        cv_.notify_all();
    }
    if (terminate) kernel_.RequestTermination(end);
}

void ThreadManager::ReapLocked(PpuThread& thread) {
    // Stack and TLS go back to lv2; the host std::thread object stays until Shutdown().
    if (thread.stack_base) kernel_.memory().Free(vm::Area::Stack, thread.stack_base);
    if (thread.tls_block) kernel_.memory().Free(vm::Area::Main, thread.tls_block);
    thread.stack_base = thread.tls_block = 0;
    threads_.erase(thread.id);  // last shared_ptr may be the caller's: thread stays valid for it
}

// ---------------------------------------------------------------------------------------
// Create / start / join / detach
// ---------------------------------------------------------------------------------------

s32 ThreadManager::Create(const CreateParams& p, u32* out_id) {
    vm::GuestMemory& m = kernel_.memory();
    if (!m.Check(p.entry_opd, 8, vm::kPageReadable)) return CELL_EFAULT;
    const u32 pc = m.Read<u32>(p.entry_opd), toc = m.Read<u32>(p.entry_opd + 4);
    if ((pc & 3) || !m.Check(pc, 4, vm::kPageReadable)) return CELL_EFAULT;
    if (p.priority < 0 || p.priority > kMaxPriority) return CELL_EINVAL;
    if (p.flags & SYS_PPU_THREAD_CREATE_INTERRUPT)
        LOG_WARN("ppu", "interrupt PPU thread requested (flags 0x%llx): created as a normal thread",
                 static_cast<unsigned long long>(p.flags));

    const u32 usable = AlignUp<u32>(std::max(p.stack_size, kMinStack), vm::k64K);
    const auto base = m.Allocate(vm::Area::Stack, u64{usable} + kStackGuard, vm::k64K, vm::kProtReadWrite);
    if (!base) return CELL_ENOMEM;
    m.Protect(*base, kStackGuard, vm::kProtNone);

    auto t = std::make_shared<PpuThread>();
    t->name = p.name;
    t->priority = p.priority;
    t->stack_base = *base;
    t->stack_size = usable + kStackGuard;
    t->tls_block = p.tls_block;
    t->joinable = (p.flags & SYS_PPU_THREAD_CREATE_JOINABLE) != 0;

    ppu::PpuThreadContext& c = t->ctx;
    const u32 top = *base + kStackGuard + usable;
    const u32 sp = AlignDown<u32>(top, 16) - kFrameHeader;
    m.Write<u64>(sp, 0);  // back chain terminator
    c.gpr[1] = sp;
    c.gpr[2] = toc;
    c.gpr[3] = p.arg;
    c.gpr[4] = p.arg2;
    c.gpr[5] = p.arg3;
    c.gpr[13] = p.tls_pointer;
    c.lr = kernel_.hle().CodeAddress(kernel_.hle().thread_exit_index());
    c.cia = pc;
    c.vscr = 0x00010000;  // VSCR[NJ]
    c.stack_addr = *base + kStackGuard;
    c.stack_size = usable;

    std::lock_guard lock(mutex_);
    if (shut_down_) {
        m.Free(vm::Area::Stack, *base);
        return CELL_EPERM;
    }
    t->id = next_id_++;
    c.thread_id = t->id;
    t->log_name = MakeLogName(t->id, t->name);
    threads_[t->id] = t;
    *out_id = t->id;
    LOG_INFO("ppu", "created thread 0x%08x \"%s\": entry 0x%08x (toc 0x%08x), arg 0x%llx, prio %d, stack 0x%x%s", t->id,
             t->name.c_str(), pc, toc, static_cast<unsigned long long>(p.arg), p.priority, usable,
             t->joinable ? ", joinable" : "");
    return CELL_OK;
}

std::optional<u64> ThreadManager::CallGuest(u32 opd, u64 a0, u64 a1, std::string name, u32 stack_size) {
    return CallGuest3(opd, a0, a1, 0, std::move(name), stack_size);
}

std::optional<u64> ThreadManager::CallGuest3(u32 opd, u64 a0, u64 a1, u64 a2, std::string name, u32 stack_size) {
    CreateParams p;
    p.entry_opd = opd;
    p.arg = a0;
    p.arg2 = a1;
    p.arg3 = a2;
    p.stack_size = stack_size;
    p.flags = SYS_PPU_THREAD_CREATE_JOINABLE;
    p.name = std::move(name);
    const auto tls = AllocateTls();
    if (!tls) return std::nullopt;
    p.tls_block = tls->first;
    p.tls_pointer = tls->second;
    u32 id = 0;
    if (Create(p, &id) != CELL_OK) {
        kernel_.memory().Free(vm::Area::Main, tls->first);
        return std::nullopt;
    }
    if (Start(id) != CELL_OK) return std::nullopt;
    ppu::PpuThreadContext host{};  // thread id 0: a host-side joiner
    u64 result = 0;
    if (Join(host, id, &result) != CELL_OK) return std::nullopt;
    return result;
}

s32 ThreadManager::Start(u32 id) {
    std::lock_guard lock(mutex_);
    const auto t = FindLocked(id);
    if (!t) return CELL_ESRCH;
    if (t->state != PpuThread::State::Created) return CELL_EBUSY;
    if (shut_down_ || kernel_.is_terminating()) return CELL_EPERM;
    t->state = PpuThread::State::Running;
    host_threads_.emplace_back(&ThreadManager::HostMain, this, t);
    return CELL_OK;
}

s32 ThreadManager::Join(const ppu::PpuThreadContext& caller, u32 id, u64* exit_value) {
    if (id == caller.thread_id) return CELL_EDEADLK;
    std::unique_lock lock(mutex_);
    const auto t = FindLocked(id);
    if (!t) return CELL_ESRCH;
    if (!t->joinable || t->joining) return CELL_EINVAL;
    t->joining = true;
    const WaitResult r = kernel_.Wait(lock, cv_, 0, [&] { return t->state == PpuThread::State::Exited; });
    t->joining = false;
    if (r != WaitResult::Ready) return CELL_ECANCELED;
    *exit_value = t->exit_value;
    ReapLocked(*t);
    return CELL_OK;
}

s32 ThreadManager::Detach(const ppu::PpuThreadContext&, u32 id) {
    std::lock_guard lock(mutex_);
    const auto t = FindLocked(id);
    if (!t) return CELL_ESRCH;
    if (!t->joinable) return CELL_EINVAL;
    if (t->joining) return CELL_EBUSY;
    t->joinable = false;
    if (t->state == PpuThread::State::Exited) ReapLocked(*t);
    return CELL_OK;
}

s32 ThreadManager::GetJoinState(const ppu::PpuThreadContext& caller, bool* joinable) {
    std::lock_guard lock(mutex_);
    const auto t = FindLocked(caller.thread_id);
    if (!t) return CELL_ESRCH;
    *joinable = t->joinable;
    return CELL_OK;
}

s32 ThreadManager::SetPriority(u32 id, s32 priority) {
    if (priority < 0 || priority > kMaxPriority) return CELL_EINVAL;
    std::lock_guard lock(mutex_);
    const auto t = FindLocked(id);
    if (!t) return CELL_ESRCH;
    t->priority = priority;  // recorded; host scheduling is not priority-mapped yet
    return CELL_OK;
}

s32 ThreadManager::GetPriority(u32 id, s32* priority) {
    std::lock_guard lock(mutex_);
    const auto t = FindLocked(id);
    if (!t) return CELL_ESRCH;
    *priority = t->priority;
    return CELL_OK;
}

s32 ThreadManager::Rename(u32 id, std::string name) {
    std::lock_guard lock(mutex_);
    const auto t = FindLocked(id);
    if (!t) return CELL_ESRCH;
    t->name = std::move(name);  // log_name keeps the creation name (a running thread points at it)
    return CELL_OK;
}

std::optional<std::pair<u32, u32>> ThreadManager::AllocateTls() {
    ProcessState& p = kernel_.process();
    vm::GuestMemory& m = kernel_.memory();
    const u32 size = 0x30 + AlignUp<u32>(p.tls_memsz, 16);
    const auto block = m.Allocate(vm::Area::Main, size, vm::k64K, vm::kProtReadWrite);
    if (!block) return std::nullopt;
    if (p.tls_filesz && m.Check(p.tls_vaddr, p.tls_filesz, vm::kPageReadable))
        std::memcpy(m.shadow() + *block + 0x30, m.shadow() + p.tls_vaddr, p.tls_filesz);
    return std::make_pair(*block, *block + 0x30 + 0x7000);
}

std::vector<ThreadManager::ThreadInfo> ThreadManager::Snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<ThreadInfo> out;
    for (const auto& [id, t] : threads_) {
        const bool done = t->state == PpuThread::State::Exited;
        ThreadInfo info{id, t->name, t->state, done ? t->stop.pc : 0, t->exit_value, t->primary,
                        done ? t->stop.reason : ppu::StopReason::None};
        const ppu::PpuLiveStatus& l = t->live;
        info.live_kind = l.kind.load(std::memory_order_acquire);
        info.live_pc = l.pc.load(std::memory_order_relaxed);
        info.live_call = l.call_id.load(std::memory_order_relaxed);
        info.live_lr = l.lr.load(std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) info.live_args[i] = l.args[i].load(std::memory_order_relaxed);
        info.live_instructions = l.instructions.load(std::memory_order_relaxed);
        info.live_calls = l.calls.load(std::memory_order_relaxed);
        const u64 done_calls = l.completed.load(std::memory_order_acquire);
        const u64 first = done_calls > ppu::PpuLiveStatus::kRecentCalls ? done_calls - ppu::PpuLiveStatus::kRecentCalls : 0;
        for (u64 n = first; n < done_calls; ++n) {
            const auto& r = l.recent[n % ppu::PpuLiveStatus::kRecentCalls];
            ppu::CallRecord rec;
            rec.pc = r.lr.load(std::memory_order_relaxed);
            rec.syscall = r.syscall.load(std::memory_order_relaxed) != 0;
            rec.id = r.id.load(std::memory_order_relaxed);
            rec.args[0] = r.arg0.load(std::memory_order_relaxed);
            rec.args[1] = r.arg1.load(std::memory_order_relaxed);
            rec.result = r.result.load(std::memory_order_relaxed);
            info.live_recent.push_back(rec);
        }
        out.push_back(std::move(info));
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// Syscalls
// ---------------------------------------------------------------------------------------

void ThreadManager::RegisterSyscalls(SyscallTable& table) {
    using C = ppu::PpuThreadContext;
#define TM static_cast<ThreadManager*>(p)
#define MEM TM->kernel_.memory()
    table.Register(43, "sys_ppu_thread_yield", [](void*, C&) -> s32 {
        std::this_thread::yield();
        return CELL_OK;
    }, this);
    table.Register(44, "sys_ppu_thread_join", [](void* p, C& c) -> s32 {
        const u32 out = static_cast<u32>(c.gpr[4]);
        if (out && !MEM.Check(out, 8, vm::kPageWritable)) return CELL_EFAULT;
        u64 value = 0;
        const s32 r = TM->Join(c, static_cast<u32>(c.gpr[3]), &value);
        if (r == CELL_OK && out) MEM.Write<u64>(out, value);
        return r;
    }, this);
    table.Register(45, "sys_ppu_thread_detach", [](void* p, C& c) -> s32 {
        return TM->Detach(c, static_cast<u32>(c.gpr[3]));
    }, this);
    table.Register(46, "sys_ppu_thread_get_join_state", [](void* p, C& c) -> s32 {
        const u32 out = static_cast<u32>(c.gpr[3]);
        if (!MEM.Check(out, 4, vm::kPageWritable)) return CELL_EFAULT;
        bool joinable = false;
        const s32 r = TM->GetJoinState(c, &joinable);
        if (r == CELL_OK) MEM.Write<u32>(out, joinable ? 1 : 0);
        return r;
    }, this);
    table.Register(47, "sys_ppu_thread_set_priority", [](void* p, C& c) -> s32 {
        return TM->SetPriority(static_cast<u32>(c.gpr[3]), static_cast<s32>(c.gpr[4]));
    }, this);
    table.Register(48, "sys_ppu_thread_get_priority", [](void* p, C& c) -> s32 {
        const u32 out = static_cast<u32>(c.gpr[4]);
        if (!MEM.Check(out, 4, vm::kPageWritable)) return CELL_EFAULT;
        s32 prio = 0;
        const s32 r = TM->GetPriority(static_cast<u32>(c.gpr[3]), &prio);
        if (r == CELL_OK) MEM.Write<u32>(out, static_cast<u32>(prio));
        return r;
    }, this);
    table.Register(49, "sys_ppu_thread_get_stack_information", [](void* p, C& c) -> s32 {
        // sys_ppu_thread_stack_t { be32 pst_addr; be32 pst_size; }
        const u32 out = static_cast<u32>(c.gpr[3]);
        if (!MEM.Check(out, 8, vm::kPageWritable)) return CELL_EFAULT;
        MEM.Write<u32>(out, c.stack_addr);
        MEM.Write<u32>(out + 4, c.stack_size);
        return CELL_OK;
    }, this);
    table.Register(52, "_sys_ppu_thread_create", [](void* p, C& c) -> s32 {
        // (u64* thread_id, ppu_thread_param_t* {be32 entry; be32 tls}, u64 arg, u64 unk,
        //  s32 prio, u32 stacksize, u64 flags, const char* name)
        const u32 out = static_cast<u32>(c.gpr[3]), param = static_cast<u32>(c.gpr[4]);
        if (!MEM.Check(out, 8, vm::kPageWritable) || !MEM.Check(param, 8, vm::kPageReadable)) return CELL_EFAULT;
        CreateParams cp;
        cp.entry_opd = MEM.Read<u32>(param);
        cp.tls_pointer = MEM.Read<u32>(param + 4);
        cp.arg = c.gpr[5];
        cp.priority = static_cast<s32>(c.gpr[7]);
        cp.stack_size = static_cast<u32>(c.gpr[8]);
        cp.flags = c.gpr[9];
        cp.name = MEM.ReadCString(static_cast<u32>(c.gpr[10]), 64).value_or("");
        u32 id = 0;
        const s32 r = TM->Create(cp, &id);
        if (r == CELL_OK) MEM.Write<u64>(out, id);
        return r;
    }, this);
    table.Register(53, "sys_ppu_thread_start", [](void* p, C& c) -> s32 {
        return TM->Start(static_cast<u32>(c.gpr[3]));
    }, this);
    table.Register(56, "sys_ppu_thread_rename", [](void* p, C& c) -> s32 {
        return TM->Rename(static_cast<u32>(c.gpr[3]), MEM.ReadCString(static_cast<u32>(c.gpr[4]), 64).value_or(""));
    }, this);
#undef MEM
#undef TM
}

}  // namespace cell::lv2
