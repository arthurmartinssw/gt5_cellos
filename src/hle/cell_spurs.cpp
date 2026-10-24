// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_spurs.cpp
#include "hle/cell_spurs.h"

#include <atomic>

#include "common/log.h"

namespace cell::hle {

namespace {

using C = ppu::PpuThreadContext;
using M = vm::GuestMemory;

CellSpurs& Self(const HleFunction& f) { return *static_cast<CellSpurs*>(f.user); }
u32 Arg32(const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); }
void Return(C& c, s64 value) { c.gpr[3] = static_cast<u64>(value); }

constexpr u32 kAttributeSize = 512;  // CellSpursAttribute, align 8
constexpr u32 kSpursAlign = 128;     // CellSpurs, align 128
constexpr u32 kMaxSpus = 6;

s32 CheckAttribute(const M& m, u32 attr) {
    if (!attr) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (attr % 8) return CELL_SPURS_CORE_ERROR_ALIGN;
    if (!m.Check(attr, kAttributeSize, vm::kPageWritable)) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    return CELL_OK;
}

// (CellSpursAttribute* attr, u32 nSpus, s32 spuPriority, s32 ppuPriority, bool exitIfNoWork)
// Mirrors the leading fields of the system library's layout (revision, sdk, nSpus,
// spuPriority, ppuPriority, exitIfNoWork) so code that inspects them sees sane values.
void AttributeInitialize(C& c, M& m, const HleFunction&) {
    const u32 attr = Arg32(c, 0), num = Arg32(c, 1);
    if (const s32 r = CheckAttribute(m, attr)) return Return(c, r);
    if (num == 0 || num > kMaxSpus) return Return(c, CELL_SPURS_CORE_ERROR_INVAL);
    m.Fill(attr, 0, kAttributeSize);
    m.Write<u32>(attr + 0, 2);  // revision
    m.Write<u32>(attr + 8, num);
    m.Write<u32>(attr + 12, Arg32(c, 2));
    m.Write<u32>(attr + 16, Arg32(c, 3));
    m.Write<u8>(attr + 20, c.gpr[7] ? 1 : 0);
    Return(c, CELL_OK);
}

// (CellSpursAttribute* attr, const char* prefix, u32 size)
void AttributeSetNamePrefix(C& c, M& m, const HleFunction&) {
    if (const s32 r = CheckAttribute(m, Arg32(c, 0))) return Return(c, r);
    if (!Arg32(c, 1)) return Return(c, CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (Arg32(c, 2) > 15) return Return(c, CELL_SPURS_CORE_ERROR_INVAL);
    Return(c, CELL_OK);
}

void AttributeAccept(C& c, M& m, const HleFunction&) {
    Return(c, CheckAttribute(m, Arg32(c, 0)));
}

// (CellSpurs* spurs, u32 nSpu, s32 spuPriority, s32 ppuPriority, bool exitIfNoWork)
void SpursInitialize(C& c, M&, const HleFunction& self) { Return(c, Self(self).Initialize(Arg32(c, 0), Arg32(c, 1))); }

// (CellSpurs* spurs, const CellSpursAttribute* attr)
void InitializeWithAttribute(C& c, M& m, const HleFunction& self) {
    const u32 attr = Arg32(c, 1);
    if (!attr) return Return(c, CELL_SPURS_CORE_ERROR_NULL_POINTER);
    if (attr % 8) return Return(c, CELL_SPURS_CORE_ERROR_ALIGN);
    if (!m.Check(attr, kAttributeSize, vm::kPageReadable)) return Return(c, CELL_SPURS_CORE_ERROR_NULL_POINTER);
    Return(c, Self(self).Initialize(Arg32(c, 0), m.Read<u32>(attr + 8)));
}

void SpursFinalize(C& c, M&, const HleFunction& self) { Return(c, Self(self).Finalize(Arg32(c, 0))); }

// (CellSpurs* spurs, u32 queue, u8* port, s32 isDynamic): SPURS forwards SPU-side events to
// the queue through a port it owns. Accepted; nothing is forwarded until SPURS runs.
void AttachLv2EventQueue(C& c, M& m, const HleFunction& self) {
    if (!Self(self).IsInitialized(Arg32(c, 0))) return Return(c, CELL_SPURS_CORE_ERROR_STAT);
    const u32 port = Arg32(c, 2);
    if (!port || !m.Check(port, 1, vm::kPageWritable)) return Return(c, CELL_SPURS_CORE_ERROR_NULL_POINTER);
    static std::atomic<u32> next{0x10};
    if (c.gpr[6]) m.Write<u8>(port, static_cast<u8>(0x10 + (next.fetch_add(1) - 0x10) % 0x30));
    LOG_WARN("cellSpurs", "cellSpursAttachLv2EventQueue(queue 0x%08x): accepted, no SPURS events will be delivered",
             Arg32(c, 1));
    Return(c, CELL_OK);
}

void SpursAccept(C& c, M&, const HleFunction& self) {
    Return(c, Self(self).IsInitialized(Arg32(c, 0)) ? CELL_OK : CELL_SPURS_CORE_ERROR_STAT);
}

void NeedsSpuRuntime(C& c, M&, const HleFunction& self) {
    LOG_ERROR("cellSpurs", "%s: SPURS work submitted, but SPU code cannot run yet", self.name.c_str());
    c.exit_kind = ppu::kExitAbort;
    c.exit_message =
        "cellSpurs work submission (taskset / task / job chain / workload) needs the SPU runtime: SPU executor "
        "(SPU ISA -> x86-64) and the host SPURS scheduler, both later phases. See the last HLE calls above.";
    c.exit_status = 0;
    c.exit_requested = true;
}

}  // namespace

s32 CellSpurs::Initialize(u32 spurs, u32 num_spus) {
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (spurs % kSpursAlign) return CELL_SPURS_CORE_ERROR_ALIGN;
    if (num_spus == 0 || num_spus > kMaxSpus) return CELL_SPURS_CORE_ERROR_INVAL;
    std::lock_guard lock(mutex_);
    if (!instances_.insert(spurs).second) return CELL_SPURS_CORE_ERROR_STAT;
    LOG_WARN("cellSpurs",
             "cellSpursInitialize(0x%08x, %u SPUs): accepted without an SPU runtime; work submitted to it will stop "
             "the process with a report",
             spurs, num_spus);
    return CELL_OK;
}

s32 CellSpurs::Finalize(u32 spurs) {
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    std::lock_guard lock(mutex_);
    return instances_.erase(spurs) ? CELL_OK : CELL_SPURS_CORE_ERROR_STAT;
}

bool CellSpurs::IsInitialized(u32 spurs) const {
    std::lock_guard lock(mutex_);
    return instances_.count(spurs) != 0;
}

void CellSpurs::Register(HleRegistry& r) {
    constexpr const char* kModule = "cellSpurs";
    struct Entry {
        const char* name;
        HleFunc fn;
    };
    static constexpr Entry kEntries[] = {
        {"cellSpursAttributeInitialize", AttributeInitialize},
        {"cellSpursAttributeSetNamePrefix", AttributeSetNamePrefix},
        {"cellSpursAttributeEnableSpuPrintfIfAvailable", AttributeAccept},
        {"cellSpursAttributeSetSpuThreadGroupType", AttributeAccept},
        {"cellSpursAttributeSetMemoryContainerForSpuThread", AttributeAccept},
        {"cellSpursAttributeEnableSystemWorkload", AttributeAccept},
        {"cellSpursInitialize", SpursInitialize},
        {"cellSpursInitializeWithAttribute", InitializeWithAttribute},
        {"cellSpursInitializeWithAttribute2", InitializeWithAttribute},
        {"cellSpursFinalize", SpursFinalize},
        {"cellSpursAttachLv2EventQueue", AttachLv2EventQueue},
        {"cellSpursDetachLv2EventQueue", SpursAccept},
        {"cellSpursSetMaxContention", SpursAccept},
        {"cellSpursSetPriorities", SpursAccept},
        {"cellSpursSetPreemptionVictimHints", SpursAccept},
        {"cellSpursCreateTaskset", NeedsSpuRuntime},
        {"cellSpursCreateTasksetWithAttribute", NeedsSpuRuntime},
        {"cellSpursCreateTask", NeedsSpuRuntime},
        {"cellSpursCreateTaskWithAttribute", NeedsSpuRuntime},
        {"cellSpursAddWorkload", NeedsSpuRuntime},
        {"cellSpursAddWorkloadWithAttribute", NeedsSpuRuntime},
        {"cellSpursCreateJobChain", NeedsSpuRuntime},
        {"cellSpursCreateJobChainWithAttribute", NeedsSpuRuntime},
    };
    for (const Entry& e : kEntries) r.Register(kModule, e.name, e.fn, this);
}

}  // namespace cell::hle
