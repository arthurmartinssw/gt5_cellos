// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/sys_prx_for_user.cpp
//
// Handler convention: arguments in ctx.gpr[3..10] (pointers are 32-bit EAs: always cast to
// u32), result in ctx.gpr[3]. `self.user` is the SysPrxForUser instance.
#include "hle/sys_prx_for_user.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "common/log.h"
#include "hle/guest_format.h"
#include "lv2/sys_mmapper.h"
#include "lv2/sys_ppu_thread.h"
#include "lv2/sys_prx.h"
#include "lv2/sys_sync.h"
#include "ppu/ppu_context.h"

namespace cell::hle {

namespace {

using C = ppu::PpuThreadContext;
using M = vm::GuestMemory;

SysPrxForUser& Self(const HleFunction& f) { return *static_cast<SysPrxForUser*>(f.user); }
u32 Arg32(const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); }
void Return(C& c, s64 value) { c.gpr[3] = static_cast<u64>(value); }

// ---------------- process ----------------
void sys_initialize_tls(C& c, M& m, const HleFunction&) {
    // (thread_id, tls_seg_addr, tls_seg_size, tls_mem_size): build the TLS block for the
    // calling thread and point r13 at it (same layout as the loader's: 0x30 TCB + 0x7000 bias).
    const u32 image = Arg32(c, 1), filesz = Arg32(c, 2), memsz = Arg32(c, 3);
    const u32 size = 0x30 + AlignUp<u32>(memsz, 16);
    const auto block = m.Allocate(vm::Area::Main, size, vm::k64K, vm::kProtReadWrite);
    if (!block) {
        LOG_ERROR("sysPrxForUser", "sys_initialize_tls: out of memory for 0x%x bytes", size);
        return;
    }
    if (filesz && m.Check(image, filesz, vm::kPageReadable))
        std::memcpy(m.shadow() + *block + 0x30, m.shadow() + image, filesz);
    c.gpr[13] = *block + 0x30 + 0x7000;
    LOG_INFO("sysPrxForUser", "sys_initialize_tls(image 0x%08x, 0x%x/0x%x) -> r13 0x%08llx", image, filesz, memsz,
             static_cast<unsigned long long>(c.gpr[13]));
}

void sys_process_exit(C& c, M&, const HleFunction&) {
    c.exit_status = c.gpr[3];
    c.exit_kind = ppu::kExitProcess;
    c.exit_requested = true;
}

std::vector<std::string> ReadArgv(const M& m, u32 argv) {
    std::vector<std::string> out;
    for (u32 i = 0; argv && i < 64 && m.Check(argv + i * 4, 4, vm::kPageReadable); ++i) {
        const u32 p = m.Read<u32>(argv + i * 4);
        if (!p) break;
        out.push_back(m.ReadCString(p, 1024).value_or("<unreadable>"));
    }
    return out;
}

// sys_game_process_exitspawn{,2}(path, argv, envp, data, data_size, prio, flags):
// replace this process with another executable. We record the request and stop the PPU;
// the front end reports which SELF the game wants next (see main.cpp).
void exitspawn(C& c, M& m, const HleFunction& self) {
    lv2::ProcessState& p = Self(self).process();
    std::lock_guard lock(p.mutex);
    p.exitspawn_requested = true;
    p.exitspawn_path = m.ReadCString(Arg32(c, 0), 1024).value_or("<unreadable>");
    p.exitspawn_args = ReadArgv(m, Arg32(c, 1));
    p.exitspawn_prio = Arg32(c, 5);
    p.exitspawn_flags = c.gpr[9];
    LOG_INFO("sysPrxForUser", "%s(\"%s\", %zu args, prio %u, flags 0x%llx)", self.name.c_str(),
             p.exitspawn_path.c_str(), p.exitspawn_args.size(), p.exitspawn_prio,
             static_cast<unsigned long long>(p.exitspawn_flags));
    c.exit_kind = ppu::kExitSpawn;
    c.exit_status = 0;
    c.exit_requested = true;
}

// sys_process_get_paramsfo(u8 buffer[0x40]): the PARAM.SFO summary lv2 keeps for the
// process; only the title id (at +1, up to 9 characters) is filled, as RPCS3 does.
void sys_process_get_paramsfo(C& c, M& m, const HleFunction& self) {
    const u32 buffer = Arg32(c, 0);
    if (!m.Check(buffer, 0x40, vm::kPageWritable)) return Return(c, CELL_EFAULT);
    const std::string& title = Self(self).process().title_id;
    if (title.empty()) return Return(c, CELL_ENOENT);
    m.Fill(buffer, 0, 0x40);
    m.CopyIn(buffer + 1, title.data(), std::min<std::size_t>(title.size(), 9));
    Return(c, CELL_OK);
}

void sys_process_is_stack(C& c, M&, const HleFunction&) {
    Return(c, M::AreaOf(Arg32(c, 0)) == vm::Area::Stack ? 1 : 0);
}

void sys_time_get_system_time(C& c, M&, const HleFunction&) {
    Return(c, static_cast<s64>(ppu::PpuInterpreter::SystemTimeMicroseconds()));
}

void sys_ppu_thread_get_id(C& c, M& m, const HleFunction&) {
    const u32 out = Arg32(c, 0);
    if (!m.Check(out, 8, vm::kPageWritable)) return Return(c, CELL_EFAULT);
    m.Write<u64>(out, c.thread_id);
    Return(c, CELL_OK);
}

void sys_ppu_thread_exit(C& c, M&, const HleFunction&) {
    c.exit_status = c.gpr[3];
    c.exit_kind = ppu::kExitThread;
    c.exit_requested = true;
}

void ok_noop(C& c, M&, const HleFunction& self) {
    LOG_TRACE("sysPrxForUser", "%s: accepted (no-op)", self.name.c_str());
    Return(c, CELL_OK);
}

// ---------------- PPU threads ----------------
// sys_ppu_thread_create(u64* thread_id, u32 entry_opd, u64 arg, s32 prio, u32 stacksize,
//                       u64 flags, const char* name)
// liblv2 allocates the new thread's TLS from the process image, then issues
// _sys_ppu_thread_create (52) and sys_ppu_thread_start (53). We do the same in one call.
void sys_ppu_thread_create(C& c, M& m, const HleFunction& self) {
    lv2::Kernel& k = Self(self).kernel();
    const u32 out = Arg32(c, 0);
    if (!m.Check(out, 8, vm::kPageWritable)) return Return(c, CELL_EFAULT);
    lv2::ThreadManager::CreateParams p;
    p.entry_opd = Arg32(c, 1);
    p.arg = c.gpr[5];
    p.priority = static_cast<s32>(c.gpr[6]);
    p.stack_size = Arg32(c, 4);
    p.flags = c.gpr[8];
    p.name = m.ReadCString(Arg32(c, 6), 64).value_or("");
    const auto tls = k.threads().AllocateTls();
    if (!tls) return Return(c, CELL_ENOMEM);
    p.tls_block = tls->first;
    p.tls_pointer = tls->second;
    u32 id = 0;
    s32 r = k.threads().Create(p, &id);
    if (r != CELL_OK) {
        m.Free(vm::Area::Main, tls->first);
        return Return(c, r);
    }
    m.Write<u64>(out, id);
    r = k.threads().Start(id);
    Return(c, r);
}

// ---------------- lightweight mutex ----------------
// sys_lwmutex_t (24 bytes): +0 owner, +4 waiter, +8 attribute, +12 recursive_count,
//                           +16 sleep_queue (our kernel object id), +20 pad
// sys_lwmutex_attribute_t: +0 protocol, +4 recursive, +8 name[8]
constexpr u32 kLwOwner = 0, kLwWaiter = 4, kLwAttr = 8, kLwCount = 12, kLwQueue = 16;

std::shared_ptr<lv2::Lv2Mutex> LwMutexOf(lv2::Kernel& k, const M& m, u32 lwmutex) {
    if (!m.Check(lwmutex, 24, vm::kPageWritable)) return nullptr;
    return k.objects().Get<lv2::Lv2Mutex>(m.Read<u32>(lwmutex + kLwQueue));
}

void sys_lwmutex_create(C& c, M& m, const HleFunction& self) {
    lv2::Kernel& k = Self(self).kernel();
    const u32 lw = Arg32(c, 0), attr = Arg32(c, 1);
    if (!m.Check(lw, 24, vm::kPageWritable) || !m.Check(attr, 16, vm::kPageReadable)) return Return(c, CELL_EFAULT);
    const u32 protocol = m.Read<u32>(attr), recursive = m.Read<u32>(attr + 4);
    if (recursive != lv2::SYS_SYNC_RECURSIVE && recursive != lv2::SYS_SYNC_NOT_RECURSIVE) return Return(c, CELL_EINVAL);
    auto mx = std::make_shared<lv2::Lv2Mutex>();
    mx->protocol = protocol;
    mx->recursive = recursive == lv2::SYS_SYNC_RECURSIVE;
    mx->name = lv2::ReadObjectName(m, attr + 8);
    mx->mirror_memory = &m;
    mx->mirror_addr = lw;
    const u32 id = k.objects().Add(lv2::kIdLwMutex, mx);
    m.Write<u32>(lw + kLwOwner, lv2::kLwmutexFree);
    m.Write<u32>(lw + kLwWaiter, 0);
    m.Write<u32>(lw + kLwAttr, protocol | recursive);
    m.Write<u32>(lw + kLwCount, 0);
    m.Write<u32>(lw + kLwQueue, id);
    Return(c, CELL_OK);
}
void sys_lwmutex_destroy(C& c, M& m, const HleFunction& self) {
    lv2::Kernel& k = Self(self).kernel();
    const u32 lw = Arg32(c, 0);
    const auto mx = LwMutexOf(k, m, lw);
    if (!mx) return Return(c, CELL_ESRCH);
    {
        std::lock_guard lock(mx->m);
        if (mx->owner || mx->waiters) return Return(c, CELL_EBUSY);
        if (mx->cond_refs) return Return(c, CELL_EPERM);
        mx->destroyed = true;
    }
    k.objects().Remove(mx->id);
    m.Write<u32>(lw + kLwQueue, 0);
    Return(c, CELL_OK);
}
void sys_lwmutex_lock(C& c, M& m, const HleFunction& self) {  // (lwmutex*, u64 timeout)
    lv2::Kernel& k = Self(self).kernel();
    const auto mx = LwMutexOf(k, m, Arg32(c, 0));
    Return(c, mx ? mx->Lock(k, c.thread_id, c.gpr[4]) : CELL_ESRCH);
}
void sys_lwmutex_trylock(C& c, M& m, const HleFunction& self) {
    const auto mx = LwMutexOf(Self(self).kernel(), m, Arg32(c, 0));
    Return(c, mx ? mx->TryLock(c.thread_id) : CELL_ESRCH);
}
void sys_lwmutex_unlock(C& c, M& m, const HleFunction& self) {
    const auto mx = LwMutexOf(Self(self).kernel(), m, Arg32(c, 0));
    Return(c, mx ? mx->Unlock(c.thread_id) : CELL_ESRCH);
}

// ---------------- lightweight condition variable ----------------
// sys_lwcond_t (8 bytes): +0 lwmutex*, +4 lwcond_queue (our kernel object id)
std::shared_ptr<lv2::Lv2Cond> LwCondOf(lv2::Kernel& k, const M& m, u32 lwcond) {
    if (!m.Check(lwcond, 8, vm::kPageReadable)) return nullptr;
    return k.objects().Get<lv2::Lv2Cond>(m.Read<u32>(lwcond + 4));
}

// A wait/signal on a sys_lwcond_t whose id resolves to no condition: a guest/host mapping
// problem (struct copied or overwritten, destroyed object, wrong pointer) that would otherwise
// be a silent ESRCH — and a signal lost that way leaves its waiter blocked forever. Logged for
// the first 16 occurrences in the process.
void ReportMissingLwCond(const char* fn, lv2::Kernel& k, const M& m, const C& c, u32 lwcond) {
    static std::atomic<u32> reported{0};
    if (reported.fetch_add(1, std::memory_order_relaxed) >= 16) return;
    const bool readable = lwcond && m.Check(lwcond, 8, vm::kPageReadable);
    const u32 lwmutex = readable ? m.Read<u32>(lwcond) : 0, id = readable ? m.Read<u32>(lwcond + 4) : 0;
    const auto obj = readable ? k.objects().Get<lv2::Lv2Object>(id) : nullptr;
    LOG_WARN("sysPrxForUser", "%s(0x%08x) -> ESRCH: %s (lwmutex field 0x%08x, id field 0x%08x%s), thread 0x%08x, caller lr 0x%08llx", fn,
             lwcond, readable ? "no lwcond with that id" : "struct not readable", lwmutex, id,
             obj ? ": the id names a different object type" : "", c.thread_id, static_cast<unsigned long long>(c.lr));
}

void sys_lwcond_create(C& c, M& m, const HleFunction& self) {  // (lwcond*, lwmutex*, attr*)
    lv2::Kernel& k = Self(self).kernel();
    const u32 lc = Arg32(c, 0), lw = Arg32(c, 1), attr = Arg32(c, 2);
    if (!m.Check(lc, 8, vm::kPageWritable)) return Return(c, CELL_EFAULT);
    const auto mx = LwMutexOf(k, m, lw);
    if (!mx) return Return(c, CELL_ESRCH);
    auto cond = std::make_shared<lv2::Lv2Cond>();
    cond->mutex = mx;
    cond->name = lv2::ReadObjectName(m, attr);
    cond->SetCreator(c.thread_id, static_cast<u32>(c.lr));
    {
        std::lock_guard lock(mx->m);
        ++mx->cond_refs;
    }
    m.Write<u32>(lc, lw);
    m.Write<u32>(lc + 4, k.objects().Add(lv2::kIdLwCond, cond));
    Return(c, CELL_OK);
}
void sys_lwcond_destroy(C& c, M& m, const HleFunction& self) {
    lv2::Kernel& k = Self(self).kernel();
    const auto cond = LwCondOf(k, m, Arg32(c, 0));
    if (!cond) return Return(c, CELL_ESRCH);
    {
        std::lock_guard lock(cond->mutex->m);
        if (!cond->waiters.empty()) return Return(c, CELL_EBUSY);
        cond->destroyed = true;
        --cond->mutex->cond_refs;
    }
    k.objects().Remove(cond->id);
    Return(c, CELL_OK);
}
void sys_lwcond_wait(C& c, M& m, const HleFunction& self) {  // (lwcond*, u64 timeout)
    lv2::Kernel& k = Self(self).kernel();
    const auto cond = LwCondOf(k, m, Arg32(c, 0));
    if (!cond) ReportMissingLwCond("sys_lwcond_wait", k, m, c, Arg32(c, 0));
    Return(c, cond ? cond->Wait(k, c.thread_id, c.gpr[4]) : CELL_ESRCH);
}
void sys_lwcond_signal(C& c, M& m, const HleFunction& self) {
    lv2::Kernel& k = Self(self).kernel();
    const auto cond = LwCondOf(k, m, Arg32(c, 0));
    if (!cond) ReportMissingLwCond("sys_lwcond_signal", k, m, c, Arg32(c, 0));
    Return(c, cond ? cond->Signal(c.thread_id) : CELL_ESRCH);
}
void sys_lwcond_signal_all(C& c, M& m, const HleFunction& self) {
    lv2::Kernel& k = Self(self).kernel();
    const auto cond = LwCondOf(k, m, Arg32(c, 0));
    if (!cond) ReportMissingLwCond("sys_lwcond_signal_all", k, m, c, Arg32(c, 0));
    Return(c, cond ? cond->SignalAll(c.thread_id) : CELL_ESRCH);
}
void sys_lwcond_signal_to(C& c, M& m, const HleFunction& self) {  // (lwcond*, u32 thread_id)
    lv2::Kernel& k = Self(self).kernel();
    const auto cond = LwCondOf(k, m, Arg32(c, 0));
    if (!cond) ReportMissingLwCond("sys_lwcond_signal_to", k, m, c, Arg32(c, 0));
    Return(c, cond ? cond->SignalTo(Arg32(c, 1), c.thread_id) : CELL_ESRCH);
}

// ---------------- libc helpers (operate on guest memory through the shadow view) ----------
bool Range(const M& m, u32 ea, u32 size, u8 need) { return !size || m.Check(ea, size, need); }
u32 GuestStrlen(const M& m, u32 s) {
    const auto str = m.ReadCString(s, 1u << 20);
    return str ? static_cast<u32>(str->size()) : 0;
}
void _sys_strlen(C& c, M& m, const HleFunction&) { Return(c, GuestStrlen(m, Arg32(c, 0))); }
void _sys_memset(C& c, M& m, const HleFunction&) {
    const u32 dst = Arg32(c, 0), size = Arg32(c, 2);
    if (Range(m, dst, size, vm::kPageWritable)) m.Fill(dst, static_cast<u8>(c.gpr[4]), size);
    Return(c, dst);
}
void _sys_memcpy(C& c, M& m, const HleFunction&) {
    const u32 dst = Arg32(c, 0), src = Arg32(c, 1), size = Arg32(c, 2);
    if (Range(m, dst, size, vm::kPageWritable) && Range(m, src, size, vm::kPageReadable))
        std::memmove(m.shadow() + dst, m.shadow() + src, size);
    Return(c, dst);
}
void _sys_memcmp(C& c, M& m, const HleFunction&) {
    const u32 a = Arg32(c, 0), b = Arg32(c, 1), size = Arg32(c, 2);
    if (!Range(m, a, size, vm::kPageReadable) || !Range(m, b, size, vm::kPageReadable)) return Return(c, 0);
    const int r = std::memcmp(m.shadow() + a, m.shadow() + b, size);
    Return(c, r < 0 ? -1 : r > 0 ? 1 : 0);
}
void _sys_strcmp(C& c, M& m, const HleFunction&) {
    const auto a = m.ReadCString(Arg32(c, 0), 1u << 20), b = m.ReadCString(Arg32(c, 1), 1u << 20);
    const int r = (a && b) ? a->compare(*b) : 0;
    Return(c, r < 0 ? -1 : r > 0 ? 1 : 0);
}
void _sys_strncmp(C& c, M& m, const HleFunction&) {
    const auto a = m.ReadCString(Arg32(c, 0), 1u << 20), b = m.ReadCString(Arg32(c, 1), 1u << 20);
    const std::size_t n = Arg32(c, 2);
    const int r = (a && b) ? a->substr(0, n).compare(b->substr(0, n)) : 0;
    Return(c, r < 0 ? -1 : r > 0 ? 1 : 0);
}
void _sys_strcpy(C& c, M& m, const HleFunction&) {
    const u32 dst = Arg32(c, 0);
    const auto s = m.ReadCString(Arg32(c, 1), 1u << 20);
    if (s && Range(m, dst, static_cast<u32>(s->size() + 1), vm::kPageWritable)) m.CopyIn(dst, s->c_str(), s->size() + 1);
    Return(c, dst);
}
void _sys_strncpy(C& c, M& m, const HleFunction&) {
    const u32 dst = Arg32(c, 0), n = Arg32(c, 2);
    const auto s = m.ReadCString(Arg32(c, 1), 1u << 20);
    if (s && Range(m, dst, n, vm::kPageWritable)) {
        const u32 copy = std::min<u32>(n, static_cast<u32>(s->size()));
        m.CopyIn(dst, s->data(), copy);
        if (n > copy) m.Fill(dst + copy, 0, n - copy);
    }
    Return(c, dst);
}
void _sys_strcat(C& c, M& m, const HleFunction&) {
    const u32 dst = Arg32(c, 0);
    const auto s = m.ReadCString(Arg32(c, 1), 1u << 20);
    const u32 at = dst + GuestStrlen(m, dst);
    if (s && Range(m, at, static_cast<u32>(s->size() + 1), vm::kPageWritable)) m.CopyIn(at, s->c_str(), s->size() + 1);
    Return(c, dst);
}
// _sys_strncat(dst, src, n): appends at most n characters of src, always NUL-terminates.
void _sys_strncat(C& c, M& m, const HleFunction&) {
    const u32 dst = Arg32(c, 0), n = Arg32(c, 2);
    auto s = m.ReadCString(Arg32(c, 1), 1u << 20);
    const u32 at = dst + GuestStrlen(m, dst);
    if (s) {
        if (s->size() > n) s->resize(n);
        if (Range(m, at, static_cast<u32>(s->size() + 1), vm::kPageWritable)) m.CopyIn(at, s->c_str(), s->size() + 1);
    }
    Return(c, dst);
}
void _sys_tolower(C& c, M&, const HleFunction&) {
    const s32 ch = static_cast<s32>(c.gpr[3]);
    Return(c, (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
}
void _sys_toupper(C& c, M&, const HleFunction&) {
    const s32 ch = static_cast<s32>(c.gpr[3]);
    Return(c, (ch >= 'a' && ch <= 'z') ? ch - 32 : ch);
}

// ---------------- printf family ----------------
void Emit(SysPrxForUser& self, const std::string& text) {
    self.process().AppendTty(text);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}
void _sys_printf(C& c, M& m, const HleFunction& self) {
    const std::string text = FormatGuest(m, c, Arg32(c, 0), 1);
    Emit(Self(self), text);
    Return(c, static_cast<s64>(text.size()));
}
void _sys_sprintf(C& c, M& m, const HleFunction&) {
    const std::string text = FormatGuest(m, c, Arg32(c, 1), 2);
    const u32 dst = Arg32(c, 0);
    if (Range(m, dst, static_cast<u32>(text.size() + 1), vm::kPageWritable)) m.CopyIn(dst, text.c_str(), text.size() + 1);
    Return(c, static_cast<s64>(text.size()));
}
// v*printf: the last argument is a va_list (pointer to 8-byte argument slots).
void _sys_vprintf(C& c, M& m, const HleFunction& self) {
    const std::string text = FormatGuestVa(m, Arg32(c, 0), Arg32(c, 1));
    Emit(Self(self), text);
    Return(c, static_cast<s64>(text.size()));
}
void _sys_vsprintf(C& c, M& m, const HleFunction&) {
    const std::string text = FormatGuestVa(m, Arg32(c, 1), Arg32(c, 2));
    const u32 dst = Arg32(c, 0);
    if (Range(m, dst, static_cast<u32>(text.size() + 1), vm::kPageWritable)) m.CopyIn(dst, text.c_str(), text.size() + 1);
    Return(c, static_cast<s64>(text.size()));
}
void _sys_vsnprintf(C& c, M& m, const HleFunction&) {
    const std::string text = FormatGuestVa(m, Arg32(c, 2), Arg32(c, 3));
    const u32 dst = Arg32(c, 0), n = Arg32(c, 1);
    if (n && Range(m, dst, n, vm::kPageWritable)) {
        const u32 copy = std::min<u32>(n - 1, static_cast<u32>(text.size()));
        m.CopyIn(dst, text.data(), copy);
        m.Fill(dst + copy, 0, 1);
    }
    Return(c, static_cast<s64>(text.size()));
}
void _sys_snprintf(C& c, M& m, const HleFunction&) {
    const std::string text = FormatGuest(m, c, Arg32(c, 2), 3);
    const u32 dst = Arg32(c, 0), n = Arg32(c, 1);
    if (n && Range(m, dst, n, vm::kPageWritable)) {
        const u32 copy = std::min<u32>(n - 1, static_cast<u32>(text.size()));
        m.CopyIn(dst, text.data(), copy);
        m.Fill(dst + copy, 0, 1);
    }
    Return(c, static_cast<s64>(text.size()));
}
void console_write(C& c, M& m, const HleFunction& self) {
    const u32 buffer = Arg32(c, 0), length = Arg32(c, 1);
    if (Range(m, buffer, length, vm::kPageReadable))
        Emit(Self(self), std::string(reinterpret_cast<const char*>(m.shadow() + buffer), length));
    Return(c, CELL_OK);
}

// ---------------- memory mapper (liblv2 wrappers over syscalls 329/332/334/335) ----------------
void MmapperAllocate(C& c, M& m, const HleFunction& self, u64 size, u64 flags, u32 out) {
    if (!m.Check(out, 4, vm::kPageWritable)) return Return(c, CELL_EFAULT);
    u32 id = 0;
    const s32 r = Self(self).kernel().mmapper().AllocateMemory(lv2::SYS_MMAPPER_NO_SHM_KEY, size, flags, &id);
    if (r == CELL_OK) m.Write<u32>(out, id);
    Return(c, r);
}
// (u32 size, u64 flags, u32* mem_id)
void sys_mmapper_allocate_memory(C& c, M& m, const HleFunction& self) {
    MmapperAllocate(c, m, self, Arg32(c, 0), c.gpr[4], Arg32(c, 2));
}
// (u32 size, u32 container, u64 flags, u32* mem_id): containers are not modelled; the
// memory comes from the process budget.
void sys_mmapper_allocate_memory_from_container(C& c, M& m, const HleFunction& self) {
    MmapperAllocate(c, m, self, Arg32(c, 0), c.gpr[5], Arg32(c, 3));
}
// (u32 addr, u32 mem_id, u64 flags)
void sys_mmapper_map_memory(C& c, M&, const HleFunction& self) {
    Return(c, Self(self).kernel().mmapper().Map(Arg32(c, 0), Arg32(c, 1), c.gpr[5]));
}
// (u32 addr, u32* mem_id)
void sys_mmapper_unmap_memory(C& c, M& m, const HleFunction& self) {
    const u32 out = Arg32(c, 1);
    if (!m.Check(out, 4, vm::kPageWritable)) return Return(c, CELL_EFAULT);
    u32 id = 0;
    const s32 r = Self(self).kernel().mmapper().Unmap(Arg32(c, 0), &id);
    if (r == CELL_OK) m.Write<u32>(out, id);
    Return(c, r);
}
// (u32 mem_id)
void sys_mmapper_free_memory(C& c, M&, const HleFunction& self) {
    Return(c, Self(self).kernel().mmapper().FreeMemory(Arg32(c, 0)));
}

// ---------------- PRX modules (user side of liblv2, on top of lv2::PrxManager) ----------------
// sys_prx_load_module(const char* path, u64 flags, opt*) -> id
void sys_prx_load_module(C& c, M& m, const HleFunction& self) {
    const auto path = m.ReadCString(Arg32(c, 0), 1024);
    if (!path) return Return(c, CELL_EFAULT);
    u32 id = 0;
    const s32 r = Self(self).kernel().prx().LoadModule(*path, &id);
    Return(c, r == CELL_OK ? static_cast<s64>(id) : r);
}
// sys_prx_start_module(id, size_t args, void* argp, int* modres, u64 flags, opt*)
// sys_prx_stop_module (same signature)
template <bool kStart>
void prx_start_stop(C& c, M& m, const HleFunction& self) {
    const u32 modres = Arg32(c, 3);
    if (modres && !m.Check(modres, 4, vm::kPageWritable)) return Return(c, CELL_EFAULT);
    s32 result = 0;
    lv2::PrxManager& prx = Self(self).kernel().prx();
    const s32 r = kStart ? prx.StartModule(Arg32(c, 0), c.gpr[4], Arg32(c, 2), &result)
                         : prx.StopModule(Arg32(c, 0), c.gpr[4], Arg32(c, 2), &result);
    if (r == CELL_OK && modres) m.Write<u32>(modres, static_cast<u32>(result));
    Return(c, r);
}
// sys_prx_unload_module(id, flags, opt*)
void sys_prx_unload_module(C& c, M&, const HleFunction& self) {
    Return(c, Self(self).kernel().prx().UnloadModule(Arg32(c, 0)));
}
// sys_prx_get_module_id_by_name(const char* name, flags, opt*) -> id
void sys_prx_get_module_id_by_name(C& c, M& m, const HleFunction& self) {
    const auto name = m.ReadCString(Arg32(c, 0), 64);
    if (!name) return Return(c, CELL_EFAULT);
    u32 id = 0;
    const s32 r = Self(self).kernel().prx().GetModuleIdByName(*name, &id);
    Return(c, r == CELL_OK ? static_cast<s64>(id) : r);
}

// sys_prx_get_module_list(u64 flags, sys_prx_get_module_list_t* info) — NID 0xA5D06BF0.
//   info: { be64 size; be32 max; be32 count; be32 idlist* }   (RPCS3 lv2/sys_prx.h)
// Contract (RPCS3 Modules/sys_prx_.cpp): EINVAL if info or info->idlist is null; otherwise the
// user library asks lv2 (_sys_prx_get_module_list, flags 2 whatever the caller passed) for up to
// info->max ids written to info->idlist, then stores max and count back into info. Always
// returns with info->count written — the field a caller iterates on.
void sys_prx_get_module_list(C& c, M& m, const HleFunction& self) {
    const u32 info = Arg32(c, 1);
    if (!info || !m.Check(info, 20, vm::kPageWritable)) return Return(c, CELL_EINVAL);
    const u32 max = m.Read<u32>(info + 8), idlist = m.Read<u32>(info + 16);
    if (!idlist) return Return(c, CELL_EINVAL);
    const std::vector<u32> ids = max ? Self(self).kernel().prx().ListModules(max) : std::vector<u32>{};
    if (!ids.empty() && !m.Check(idlist, static_cast<u32>(ids.size() * 4), vm::kPageWritable)) return Return(c, CELL_EFAULT);
    std::string text;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        m.Write<u32>(idlist + static_cast<u32>(i * 4), ids[i]);
        char id[16];
        std::snprintf(id, sizeof(id), " 0x%08x", ids[i]);
        text += id;
    }
    m.Write<u32>(info + 8, max);
    m.Write<u32>(info + 12, static_cast<u32>(ids.size()));
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
        LOG_INFO("sysPrxForUser", "sys_prx_get_module_list(flags 0x%llx, max %u) -> %zu PRX module(s):%s (HLE libraries are not PRXes)",
                 static_cast<unsigned long long>(c.gpr[3]), max, ids.size(), text.c_str());
    Return(c, CELL_OK);
}

// ---------------- sys_mempool (liblv2 fixed-size block pools) ----------------
// Contract as RPCS3 reimplements it (Modules/sys_mempool.cpp), NID 0xCA9A60BF for create:
//   sys_mempool_create(sys_mempool_t* mempool, void* chunk, u64 chunk_size, u64 block_size,
//                      u64 ralignment)
//     EINVAL: block_size > chunk_size; alignment (0 and 2 mean 4) not a power of two; chunk not
//     8-byte aligned. The pool holds chunk_size / block_size blocks at chunk + i * block_size
//     (the alignment is recorded, not applied — as in RPCS3). *mempool = pool id (1, 2, ...).
//   sys_mempool_allocate_block(pool) -> block, waiting while none is free (0 if the pool is
//     invalid or destroyed meanwhile); try_allocate_block -> block or 0, never waits. The most
//     recently freed / highest block is handed out first.
//   sys_mempool_free_block(pool, block): EINVAL bad pool or block > chunk + chunk_size; wakes
//     one waiter. sys_mempool_get_count(pool) -> free blocks (EINVAL bad pool).
//   sys_mempool_destroy(pool): void; wakes allocators, which get 0.
// Additions where RPCS3 has undefined behaviour: block_size 0 or a chunk past 4 GiB -> EINVAL
// (RPCS3 divides by zero / overflows); an unwritable *mempool -> EFAULT (RPCS3 writes through
// it unchecked). On the console liblv2 builds this on sys_mutex + sys_cond; here it is one host
// mutex + condition variable, and the pool is a kernel-table object so termination wakes waiters.
struct MemPool : lv2::Lv2Object {
    std::mutex m;
    std::condition_variable cv;
    u32 chunk = 0;
    u64 chunk_size = 0, block_size = 0, alignment = 0;
    std::vector<u32> free_blocks;  // ascending at creation; allocation pops the back
    u64 total = 0;
    u32 waiters = 0;
    bool destroyed = false;

    void WakeForTermination() override {
        std::lock_guard lock(m);
        cv.notify_all();
    }
    std::string Describe() override {
        std::unique_lock lock(m, std::try_to_lock);
        char line[192];
        if (!lock) std::snprintf(line, sizeof(line), "mempool %u: busy", id);
        else if (!waiters) return {};
        else
            std::snprintf(line, sizeof(line), "mempool %u: chunk 0x%08x, %zu of %llu block(s) of 0x%llx free, %u waiter(s)", id, chunk,
                          free_blocks.size(), static_cast<unsigned long long>(total), static_cast<unsigned long long>(block_size), waiters);
        return line;
    }
};

std::shared_ptr<MemPool> PoolOf(lv2::Kernel& k, u32 id) { return id ? k.objects().Get<MemPool>(id) : nullptr; }

void sys_mempool_create(C& c, M& m, const HleFunction& self) {
    lv2::Kernel& k = Self(self).kernel();
    const u32 out = Arg32(c, 0), chunk = Arg32(c, 1);
    const u64 chunk_size = c.gpr[5], block_size = c.gpr[6];
    u64 alignment = c.gpr[7];
    if (block_size > chunk_size) return Return(c, CELL_EINVAL);
    if (alignment == 0 || alignment == 2) alignment = 4;
    if (alignment & (alignment - 1)) return Return(c, CELL_EINVAL);
    if (chunk & 7) return Return(c, CELL_EINVAL);
    if (!block_size || u64{chunk} + chunk_size > 0x1'0000'0000ull) return Return(c, CELL_EINVAL);
    if (!out || !m.Check(out, 4, vm::kPageWritable)) return Return(c, CELL_EFAULT);
    auto pool = std::make_shared<MemPool>();
    pool->chunk = chunk;
    pool->chunk_size = chunk_size;
    pool->block_size = block_size;
    pool->alignment = alignment;
    pool->total = chunk_size / block_size;
    pool->free_blocks.reserve(static_cast<std::size_t>(pool->total));
    for (u64 i = 0; i < pool->total; ++i) pool->free_blocks.push_back(static_cast<u32>(chunk + i * block_size));
    const u32 id = k.objects().Add(0, pool);  // ids 1, 2, ... as RPCS3 numbers them
    m.Write<u32>(out, id);
    LOG_INFO("sysPrxForUser", "sys_mempool_create(chunk 0x%08x, size 0x%llx, block 0x%llx, align %llu) -> pool %u, %llu block(s)", chunk,
             static_cast<unsigned long long>(chunk_size), static_cast<unsigned long long>(block_size),
             static_cast<unsigned long long>(alignment), id, static_cast<unsigned long long>(pool->total));
    Return(c, CELL_OK);
}

void sys_mempool_destroy(C& c, M&, const HleFunction& self) {
    lv2::Kernel& k = Self(self).kernel();
    if (const auto pool = PoolOf(k, Arg32(c, 0))) {
        {
            std::lock_guard lock(pool->m);
            pool->destroyed = true;
            pool->cv.notify_all();
        }
        k.objects().Remove(pool->id);
    } else {
        LOG_WARN("sysPrxForUser", "sys_mempool_destroy(%u): no such pool", Arg32(c, 0));
    }
    Return(c, 0);  // void
}

void sys_mempool_allocate_block(C& c, M&, const HleFunction& self) {
    lv2::Kernel& k = Self(self).kernel();
    const auto pool = PoolOf(k, Arg32(c, 0));
    if (!pool) return Return(c, 0);
    std::unique_lock lock(pool->m);
    ++pool->waiters;
    const lv2::WaitResult r = k.Wait(lock, pool->cv, 0, [&] { return pool->destroyed || !pool->free_blocks.empty(); });
    --pool->waiters;
    if (r != lv2::WaitResult::Ready || pool->destroyed) return Return(c, 0);
    const u32 block = pool->free_blocks.back();
    pool->free_blocks.pop_back();
    Return(c, block);
}

void sys_mempool_try_allocate_block(C& c, M&, const HleFunction& self) {
    const auto pool = PoolOf(Self(self).kernel(), Arg32(c, 0));
    if (!pool) return Return(c, 0);
    std::lock_guard lock(pool->m);
    if (pool->destroyed || pool->free_blocks.empty()) return Return(c, 0);
    const u32 block = pool->free_blocks.back();
    pool->free_blocks.pop_back();
    Return(c, block);
}

void sys_mempool_free_block(C& c, M&, const HleFunction& self) {
    const auto pool = PoolOf(Self(self).kernel(), Arg32(c, 0));
    if (!pool) return Return(c, CELL_EINVAL);
    const u32 block = Arg32(c, 1);
    std::lock_guard lock(pool->m);
    if (pool->destroyed) return Return(c, CELL_EINVAL);
    if (u64{block} > u64{pool->chunk} + pool->chunk_size) return Return(c, CELL_EINVAL);
    pool->free_blocks.push_back(block);
    pool->cv.notify_all();  // waiters re-check under the lock; notify_all survives a timed-out peer
    Return(c, CELL_OK);
}

void sys_mempool_get_count(C& c, M&, const HleFunction& self) {
    const auto pool = PoolOf(Self(self).kernel(), Arg32(c, 0));
    if (!pool) return Return(c, CELL_EINVAL);
    std::lock_guard lock(pool->m);
    Return(c, static_cast<s64>(pool->free_blocks.size()));
}

}  // namespace

void SysPrxForUser::Register(HleRegistry& r) {
    constexpr const char* kModule = "sysPrxForUser";
    struct Entry {
        const char* name;
        HleFunc fn;
    };
    static constexpr Entry kEntries[] = {
        {"sys_initialize_tls", sys_initialize_tls},
        {"sys_process_exit", sys_process_exit},
        {"sys_game_process_exitspawn", exitspawn},
        {"sys_game_process_exitspawn2", exitspawn},
        {"_sys_process_atexitspawn", ok_noop},
        {"_sys_process_at_Exitspawn", ok_noop},
        // Unloads PRXes above a level as part of exit; the sys_process_exit that follows ends us.
        {"sys_prx_exitspawn_with_level", ok_noop},
        {"sys_process_is_stack", sys_process_is_stack},
        {"sys_time_get_system_time", sys_time_get_system_time},
        {"sys_ppu_thread_get_id", sys_ppu_thread_get_id},
        {"sys_ppu_thread_exit", sys_ppu_thread_exit},
        {"sys_ppu_thread_create", sys_ppu_thread_create},
        {"sys_prx_load_module", sys_prx_load_module},
        {"sys_prx_start_module", prx_start_stop<true>},
        {"sys_prx_stop_module", prx_start_stop<false>},
        {"sys_prx_unload_module", sys_prx_unload_module},
        {"sys_prx_get_module_id_by_name", sys_prx_get_module_id_by_name},
        {"sys_prx_get_module_list", sys_prx_get_module_list},
        {"sys_mempool_create", sys_mempool_create},
        {"sys_mempool_destroy", sys_mempool_destroy},
        {"sys_mempool_allocate_block", sys_mempool_allocate_block},
        {"sys_mempool_try_allocate_block", sys_mempool_try_allocate_block},
        {"sys_mempool_free_block", sys_mempool_free_block},
        {"sys_mempool_get_count", sys_mempool_get_count},
        {"sys_mmapper_allocate_memory", sys_mmapper_allocate_memory},
        {"sys_mmapper_allocate_memory_from_container", sys_mmapper_allocate_memory_from_container},
        {"sys_mmapper_map_memory", sys_mmapper_map_memory},
        {"sys_mmapper_unmap_memory", sys_mmapper_unmap_memory},
        {"sys_mmapper_free_memory", sys_mmapper_free_memory},
        {"sys_lwmutex_create", sys_lwmutex_create},
        {"sys_lwmutex_destroy", sys_lwmutex_destroy},
        {"sys_lwmutex_lock", sys_lwmutex_lock},
        {"sys_lwmutex_trylock", sys_lwmutex_trylock},
        {"sys_lwmutex_unlock", sys_lwmutex_unlock},
        {"sys_lwcond_create", sys_lwcond_create},
        {"sys_lwcond_destroy", sys_lwcond_destroy},
        {"sys_lwcond_wait", sys_lwcond_wait},
        {"sys_lwcond_signal", sys_lwcond_signal},
        {"sys_lwcond_signal_all", sys_lwcond_signal_all},
        {"sys_lwcond_signal_to", sys_lwcond_signal_to},
        {"_sys_strlen", _sys_strlen},
        {"_sys_memset", _sys_memset},
        {"_sys_memcpy", _sys_memcpy},
        {"_sys_memmove", _sys_memcpy},
        {"_sys_memcmp", _sys_memcmp},
        {"_sys_strcmp", _sys_strcmp},
        {"_sys_strncmp", _sys_strncmp},
        {"_sys_strcpy", _sys_strcpy},
        {"_sys_strncpy", _sys_strncpy},
        {"_sys_strcat", _sys_strcat},
        {"_sys_tolower", _sys_tolower},
        {"_sys_toupper", _sys_toupper},
        {"_sys_printf", _sys_printf},
        {"_sys_sprintf", _sys_sprintf},
        {"_sys_snprintf", _sys_snprintf},
        {"_sys_vprintf", _sys_vprintf},
        {"_sys_vsprintf", _sys_vsprintf},
        {"_sys_vsnprintf", _sys_vsnprintf},
        {"_sys_strncat", _sys_strncat},
        {"sys_process_get_paramsfo", sys_process_get_paramsfo},
        {"console_write", console_write},
    };
    for (const Entry& e : kEntries) r.Register(kModule, e.name, e.fn, this);
}

}  // namespace cell::hle
