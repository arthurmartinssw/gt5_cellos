// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/gcm_interrupts.cpp
#include "hle/gcm_interrupts.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>

#include "common/log.h"
#include "lv2/sys_ppu_thread.h"

namespace cell::hle {

namespace {

// PowerPC encodings used by the trampoline (big-endian words as stored in guest memory).
constexpr u32 Mflr(u32 rt) { return 0x7C0802A6u | rt << 21; }
constexpr u32 Mtlr(u32 rs) { return 0x7C0803A6u | rs << 21; }
constexpr u32 Mtctr(u32 rs) { return 0x7C0903A6u | rs << 21; }
constexpr u32 Lis(u32 rt, u32 imm) { return 0x3C000000u | rt << 21 | (imm & 0xFFFF); }
constexpr u32 Ori(u32 ra, u32 rs, u32 imm) { return 0x60000000u | rs << 21 | ra << 16 | (imm & 0xFFFF); }
constexpr u32 Lwz(u32 rt, u32 d, u32 ra) { return 0x80000000u | rt << 21 | ra << 16 | (d & 0xFFFF); }
constexpr u32 Mr(u32 ra, u32 rs) { return 0x7C000378u | rs << 21 | ra << 16 | rs << 11; }
constexpr u32 Li(u32 rt, u32 imm) { return 0x38000000u | rt << 21 | (imm & 0xFFFF); }
constexpr u32 Cmpwi0(u32 ra) { return 0x2C000000u | ra << 16; }
constexpr u32 Beq(s32 offset) { return 0x41820000u | (static_cast<u32>(offset) & 0xFFFC); }
constexpr u32 B(s32 offset) { return 0x48000000u | (static_cast<u32>(offset) & 0x03FFFFFC); }
constexpr u32 kBctrl = 0x4E800421u;
constexpr u32 kBlr = 0x4E800020u;
static_assert(Mflr(31) == 0x7FE802A6u && Mtlr(31) == 0x7FE803A6u && Mtctr(12) == 0x7D8903A6u && Mtctr(0) == 0x7C0903A6u);
static_assert(Mr(3, 4) == 0x7C832378u && Lwz(2, 4, 3) == 0x80430004u && Ori(12, 12, 0x1234) == 0x618C1234u);

constexpr u32 kOpdOffset = 0x100;  // OPD of the trampoline inside its 64 KiB block

}  // namespace

const char* GcmEventName(GcmEvent kind) noexcept {
    static constexpr const char* kNames[] = {"vblank", "flip", "queue", "user"};
    return kNames[static_cast<std::size_t>(kind)];
}

u64 GcmInterrupts::Now() noexcept {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

std::array<u32, GcmInterrupts::kTrampolineWords> GcmInterrupts::Trampoline(u32 wait_code) {
    return {
        Mflr(31),                    // +0   keep the thread-exit LR (r31 is callee-saved by handlers)
        Lis(12, wait_code >> 16),    // +4   loop: (HLE thunks live in the main area, < 0x80000000: no sign issue)
        Ori(12, 12, wait_code),      // +8
        Mtctr(12),                   // +12
        kBctrl,                      // +16  r3 = handler OPD (0 = exit), r4 = argument
        Cmpwi0(3),                   // +20
        Beq(52 - 24),                // +24  -> +52
        Lwz(0, 0, 3),                // +28  handler code
        Lwz(2, 4, 3),                // +32  handler TOC
        Mtctr(0),                    // +36
        Mr(3, 4),                    // +40  r3 = argument (head 1, or the user command cause)
        kBctrl,                      // +44  handler(r3)
        B(4 - 48),                   // +48  -> loop
        Li(3, 0),                    // +52
        Mtlr(31),                    // +56
        kBlr,                        // +60  return from the entry: sys_ppu_thread_exit(0)
    };
}

// ---------------------------------------------------------------------------------------
// Handlers and thread
// ---------------------------------------------------------------------------------------

void GcmInterrupts::SetHandler(GcmEvent kind, u32 opd) {
    {
        std::lock_guard lock(m_);
        handlers_[static_cast<std::size_t>(kind)] = opd;
        if (kind == GcmEvent::VBlank && opd) last_vblank_ = VBlankIndex(Now());  // first call at the next boundary
        changed_ = true;
    }
    cv_.notify_all();
}

u32 GcmInterrupts::handler(GcmEvent kind) const {
    std::lock_guard lock(m_);
    return handlers_[static_cast<std::size_t>(kind)];
}

u32 GcmInterrupts::thread_id() const {
    std::lock_guard lock(m_);
    return thread_id_;
}

bool GcmInterrupts::StartThread(u32 wait_code) {
    {
        std::lock_guard lock(m_);
        if (thread_id_ || closed_) return thread_id_ != 0;
    }
    vm::GuestMemory& m = kernel_.memory();
    const auto block = m.Allocate(vm::Area::Main, vm::k64K, vm::k64K, vm::kProtRead);
    if (!block) {
        LOG_ERROR("cellGcmSys", "_gcm_intr_thread: no guest memory for the interrupt trampoline");
        return false;
    }
    const auto words = Trampoline(wait_code);
    for (u32 i = 0; i < kTrampolineWords; ++i) m.Write<u32>(*block + 4 * i, words[i]);
    m.Write<u32>(*block + kOpdOffset, *block);  // OPD.code
    m.Write<u32>(*block + kOpdOffset + 4, 0);   // OPD.toc: the trampoline does not use r2
    m.MarkCode(*block, vm::k64K);

    lv2::ThreadManager& threads = kernel_.threads();
    const auto tls = threads.AllocateTls();  // handlers are game code: r13 must be a valid TLS pointer
    if (!tls) {
        m.Free(vm::Area::Main, *block);
        LOG_ERROR("cellGcmSys", "_gcm_intr_thread: no guest memory for TLS");
        return false;
    }
    lv2::ThreadManager::CreateParams p;
    p.entry_opd = *block + kOpdOffset;
    p.tls_block = tls->first;
    p.tls_pointer = tls->second;
    p.priority = 1;          // as libgcm / RPCS3 create it
    p.stack_size = 0x4000;   // likewise (the thread manager rounds up to 64 KiB)
    p.flags = 0;             // created as a plain thread: lv2 interrupt-thread binding is not modelled
    p.name = "_gcm_intr_thread";
    u32 id = 0;
    if (threads.Create(p, &id) != CELL_OK) {
        m.Free(vm::Area::Main, tls->first);
        m.Free(vm::Area::Main, *block);
        LOG_ERROR("cellGcmSys", "_gcm_intr_thread: thread creation failed");
        return false;
    }
    {
        std::lock_guard lock(m_);
        thread_id_ = id;
        code_ea_ = *block;
    }
    if (threads.Start(id) != CELL_OK) {
        LOG_ERROR("cellGcmSys", "_gcm_intr_thread 0x%08x: start failed", id);
        return false;
    }
    LOG_INFO("cellGcmSys", "_gcm_intr_thread 0x%08x started (trampoline 0x%08x): delivers vblank/flip/queue/user handlers", id,
             *block);
    return true;
}

void GcmInterrupts::Close() {
    {
        std::lock_guard lock(m_);
        closed_ = true;
    }
    cv_.notify_all();
}

void GcmInterrupts::WakeForTermination() {
    std::lock_guard lock(m_);
    cv_.notify_all();
}

// ---------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------

void GcmInterrupts::Post(GcmEvent kind, u64 arg) {
    const std::size_t k = static_cast<std::size_t>(kind);
    {
        std::lock_guard lock(m_);
        if (closed_ || !thread_id_) return;
        if (!handlers_[k]) {
            ++counters_.no_handler[k];
            return;
        }
        if (queue_.size() >= kMaxQueued) {
            ++counters_.overflow[k];
            return;
        }
        queue_.push_back({kind, arg});
    }
    cv_.notify_all();
}

void GcmInterrupts::OnFlipCommand(u32) { Post(GcmEvent::Queue, 1); }
void GcmInterrupts::OnFlipDone(u32) { Post(GcmEvent::Flip, 1); }
void GcmInterrupts::OnUserCommand(u32 cause) { Post(GcmEvent::User, cause); }

std::pair<u32, u64> GcmInterrupts::WaitNext() {
    std::unique_lock lock(m_);
    for (;;) {
        if (closed_ || kernel_.is_terminating()) return {0, 0};
        while (!queue_.empty()) {
            const Pending p = queue_.front();
            queue_.pop_front();
            const std::size_t k = static_cast<std::size_t>(p.kind);
            if (!handlers_[k]) {
                ++counters_.disabled[k];
                continue;
            }
            ++counters_.delivered[k];
            last_delivery_us_ = Now();
            return {handlers_[k], p.arg};
        }
        u64 timeout_us = 0;  // 0 = until notified
        if (const u32 vblank = handlers_[static_cast<std::size_t>(GcmEvent::VBlank)]) {
            const u64 now = Now();
            const u64 index = VBlankIndex(now);
            if (index > last_vblank_) {
                counters_.vblank_missed += index - last_vblank_ - 1;
                last_vblank_ = index;
                ++counters_.delivered[static_cast<std::size_t>(GcmEvent::VBlank)];
                last_delivery_us_ = now;
                return {vblank, 1};
            }
            timeout_us = std::max<u64>(VBlankTime(index + 1) - std::min(now, VBlankTime(index + 1)), 1);
        }
        changed_ = false;
        waiting_ = true;
        kernel_.Wait(lock, cv_, timeout_us, [&] { return closed_ || changed_ || !queue_.empty(); });
        waiting_ = false;
    }
}

GcmInterrupts::Counters GcmInterrupts::counters() const {
    std::lock_guard lock(m_);
    Counters c = counters_;
    c.queued = queue_.size();
    return c;
}

std::string GcmInterrupts::Describe() {
    std::unique_lock lock(m_, std::try_to_lock);
    if (!lock) return "gcm interrupts: busy";
    if (!thread_id_) return {};
    std::string out;
    char part[160];
    std::snprintf(part, sizeof(part), "gcm interrupts (thread 0x%08x \"_gcm_intr_thread\", %s): ", thread_id_,
                  closed_ ? "closed" : waiting_ ? "waiting for an event" : "outside its wait: starting or running a handler");
    out += part;
    for (std::size_t k = 0; k < kGcmEventKinds; ++k) {
        const char* name = GcmEventName(static_cast<GcmEvent>(k));
        if (handlers_[k])
            std::snprintf(part, sizeof(part), "%s%s 0x%08x x%" PRIu64, k ? "; " : "", name, handlers_[k], counters_.delivered[k]);
        else
            std::snprintf(part, sizeof(part), "%s%s none x%" PRIu64, k ? "; " : "", name, counters_.delivered[k]);
        out += part;
        if (counters_.no_handler[k] || counters_.overflow[k] || counters_.disabled[k]) {
            std::snprintf(part, sizeof(part), " (%" PRIu64 " without handler, %" PRIu64 " dropped)", counters_.no_handler[k],
                          counters_.overflow[k] + counters_.disabled[k]);
            out += part;
        }
    }
    const u64 now = Now();
    std::snprintf(part, sizeof(part), "; %" PRIu64 " vblank(s) missed; %zu queued; last call %s", counters_.vblank_missed, queue_.size(),
                  last_delivery_us_ ? "" : "never");
    out += part;
    if (last_delivery_us_) {
        std::snprintf(part, sizeof(part), "%.1f ms ago", static_cast<double>(now - std::min(now, last_delivery_us_)) / 1000.0);
        out += part;
    }
    return out;
}

}  // namespace cell::hle
