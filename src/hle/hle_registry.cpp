// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/hle_registry.cpp
#include "hle/hle_registry.h"

#include <cstdio>
#include <stdexcept>

#include "common/log.h"
#include "hle/fnid.h"
#include "ppu/ppu_disasm.h"

namespace cell::hle {

namespace {

// Default for imports without a host implementation: log the first call, return CELL_OK.
// Same policy as an unresolved NID in the PS4 runtime, except we never crash on the call:
// games often call optional APIs during boot and only check the result.
void Unimplemented(ppu::PpuThreadContext& ctx, vm::GuestMemory& memory, const HleFunction& self) {
    if (self.calls.load(std::memory_order_relaxed) == 1) {
        const std::string site = DescribeCallSite(memory, ctx.lr);
        LOG_WARN("hle", "unimplemented %s::0x%08X%s%s called (LR=0x%08llx, r3=0x%llx r4=0x%llx r5=0x%llx r6=0x%llx)%s%s",
                 self.module.c_str(), self.nid, self.name.empty() ? "" : " ", self.name.c_str(),
                 static_cast<unsigned long long>(ctx.lr), static_cast<unsigned long long>(ctx.gpr[3]),
                 static_cast<unsigned long long>(ctx.gpr[4]), static_cast<unsigned long long>(ctx.gpr[5]),
                 static_cast<unsigned long long>(ctx.gpr[6]), site.empty() ? "" : ", caller ", site.c_str());
    }
    ctx.gpr[3] = static_cast<u64>(CELL_OK);
}

// LR target of the primary thread: returning from main() == sys_process_exit(r3).
void ProcessExitTrampoline(ppu::PpuThreadContext& ctx, vm::GuestMemory&, const HleFunction&) {
    ctx.exit_status = ctx.gpr[3];
    ctx.exit_kind = ppu::kExitProcess;
    ctx.exit_requested = true;
    LOG_INFO("hle", "primary thread returned from entry, status=0x%llx",
             static_cast<unsigned long long>(ctx.exit_status));
}

// LR target of every other PPU thread: returning from the entry == sys_ppu_thread_exit(r3).
void ThreadExitTrampoline(ppu::PpuThreadContext& ctx, vm::GuestMemory&, const HleFunction&) {
    ctx.exit_status = ctx.gpr[3];
    ctx.exit_kind = ppu::kExitThread;
    ctx.exit_requested = true;
}

}  // namespace

std::string DescribeCallSite(const vm::GuestMemory& memory, u64 lr) {
    const u32 at = static_cast<u32>(lr) - 4;
    if (lr < 4 || lr > 0xFFFFFFFFull || (at & 3) || !memory.Check(at, 4, vm::kPageReadable)) return {};
    char text[32];
    std::snprintf(text, sizeof(text), "0x%08x: ", at);
    return text + ppu::Disassemble(at, memory.Read<u32>(at));
}

HleRegistry::HleRegistry(vm::GuestMemory& memory)
    : memory_(memory), slots_(std::make_unique<std::unique_ptr<HleFunction>[]>(kMaxFunctions)) {
    std::lock_guard lock(mutex_);
    exit_index_ = AddLocked("$internal", 0, "process_exit_trampoline", ProcessExitTrampoline, true, nullptr);
    thread_exit_index_ = AddLocked("$internal", 1, "thread_exit_trampoline", ThreadExitTrampoline, true, nullptr);
}

std::string HleRegistry::Key(std::string_view module, u32 nid) {
    char suffix[12];
    std::snprintf(suffix, sizeof(suffix), ":%08X", nid);
    return std::string(module) + suffix;
}

u32 HleRegistry::AddLocked(std::string module, u32 nid, std::string name, HleFunc fn, bool implemented, void* user) {
    const u32 index = count_.load(std::memory_order_relaxed);
    if (index >= kMaxFunctions) throw std::runtime_error("HLE function table full");
    slots_[index] = std::make_unique<HleFunction>(std::move(module), nid, std::move(name), fn, implemented, user);
    count_.store(index + 1, std::memory_order_release);  // publish
    return index;
}

void HleRegistry::Register(std::string_view module, std::string_view name, HleFunc fn, void* user) {
    Register(module, ComputeFnid(name), name, fn, user);
}

void HleRegistry::Register(std::string_view module, u32 nid, std::string_view name, HleFunc fn, void* user) {
    std::lock_guard lock(mutex_);
    const std::string key = Key(module, nid);
    if (const auto it = lookup_.find(key); it != lookup_.end()) {
        // Upgrading a stub that was bound before its implementation was registered.
        HleFunction& f = *slots_[it->second];
        f.fn = fn;
        f.name = name;
        f.implemented = true;
        f.user = user;
        return;
    }
    lookup_.emplace(key, AddLocked(std::string(module), nid, std::string(name), fn, true, user));
}

u32 HleRegistry::Bind(std::string_view module, u32 nid) {
    std::lock_guard lock(mutex_);
    const std::string key = Key(module, nid);
    if (const auto it = lookup_.find(key); it != lookup_.end()) return it->second;
    // Unknown to us: logging stub, named from the FNID table when the export name is known.
    const char* known = LookupFunctionName(nid);
    const u32 index = AddLocked(std::string(module), nid, known ? known : "", Unimplemented, false, nullptr);
    lookup_.emplace(key, index);
    return index;
}

void HleRegistry::Materialize() {
    std::lock_guard lock(mutex_);
    const u32 total = count_.load(std::memory_order_relaxed);
    for (u32 i = placed_; i < total; ++i) {
        const u32 chunk = i / kThunksPerChunk;
        if (chunk >= chunks_.size()) {
            // Thunks live in the main area so a plain 24-bit relative branch could reach them
            // from the EBOOT if we ever patch call sites directly.
            const auto ea = memory_.Allocate(vm::Area::Main, vm::k64K, vm::k64K, vm::kProtRead);
            if (!ea) throw std::runtime_error("cannot allocate HLE thunk chunk in main area");
            memory_.MarkCode(*ea, vm::k64K);  // the JIT translates these like guest code
            chunks_.push_back(*ea);
        }
        const u32 ea = chunks_[chunk] + (i % kThunksPerChunk) * kThunkSize;
        memory_.Write<u32>(ea + 0, kHleOpcode | i);
        memory_.Write<u32>(ea + 4, kPpcBlr);
        memory_.Write<u32>(ea + 8, ea);  // OPD.code
        memory_.Write<u32>(ea + 12, 0);  // OPD.toc: handlers do not use r2
        slots_[i]->thunk = ea;
    }
    placed_ = total;
}

void HleRegistry::Dispatch(u32 index, ppu::PpuThreadContext& ctx) {
    // No lock: slots never move and index came from a materialized thunk (< count_).
    const HleFunction& f = *slots_[index];
    f.calls.fetch_add(1, std::memory_order_relaxed);
    f.fn(ctx, memory_, f);
}

}  // namespace cell::hle
