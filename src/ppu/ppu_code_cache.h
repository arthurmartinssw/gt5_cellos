// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — ppu/ppu_code_cache.h
//
// Predecoded guest code shared by every PPU thread (phase 3). In phase 2 each interpreter
// owned its own page table; with N host threads that would mean N decodes of the same code
// and N invalidation targets. Now:
//
//   * pages_[p] is an atomic pointer. Readers (any PPU thread) do one acquire load per
//     instruction; a miss decodes the page privately and publishes it with a CAS, so two
//     threads racing on the same page waste one decode at worst and never see half a page.
//   * Invalidate() bumps the page's generation, then swaps the pointer to null; Decode()
//     re-checks the generation after publishing so a store racing with a decode can never
//     leave a stale page behind.
//   * The swapped-out page cannot be freed right away: another thread may have loaded the
//     pointer a few nanoseconds earlier and still be reading its slot. Retired pages go to a list freed at shutdown. SMC/overlay loads are rare, so the
//     list stays small; a proper epoch scheme can replace it if a game proves otherwise.
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "memory/guest_memory.h"
#include "ppu/ppu_context.h"
#include "ppu/ppu_instructions.h"
#include "ppu/ppu_opcode.h"

namespace cell::ppu {

class PpuInterpreter;
using PpuHandler = void (*)(PpuInterpreter&, PpuThreadContext&, Op);

struct DecodedPage {
    PpuHandler handler[1024];
    u32 raw[1024];
    PpuInstr id[1024];
};

// Handler table generated in ppu_interpreter.cpp from PPU_INSTRUCTION_LIST.
[[nodiscard]] const std::array<PpuHandler, kPpuInstrCount>& PpuHandlerTable() noexcept;

class PpuCodeCache {
public:
    explicit PpuCodeCache(vm::GuestMemory& memory);
    ~PpuCodeCache();
    PpuCodeCache(const PpuCodeCache&) = delete;
    PpuCodeCache& operator=(const PpuCodeCache&) = delete;

    [[nodiscard]] DecodedPage* Lookup(u32 page) const noexcept { return pages_[page].load(std::memory_order_acquire); }
    // Decode and publish; nullptr if the page is not readable guest memory.
    DecodedPage* Decode(u32 page) noexcept;
    void Invalidate(u32 addr, u64 size) noexcept;
    [[nodiscard]] std::size_t retired_pages() const;

private:
    vm::GuestMemory& memory_;
    std::unique_ptr<std::atomic<DecodedPage*>[]> pages_;
    std::unique_ptr<std::atomic<u32>[]> gens_;  // bumped by every invalidation of the page
    mutable std::mutex retire_mutex_;
    std::vector<std::unique_ptr<DecodedPage>> retired_;
};

}  // namespace cell::ppu
