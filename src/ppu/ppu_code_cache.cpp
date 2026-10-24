// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — ppu/ppu_code_cache.cpp
#include "ppu/ppu_code_cache.h"

#include <atomic>
#include <cstring>

#include "ppu/ppu_decoder.h"

namespace cell::ppu {

PpuCodeCache::PpuCodeCache(vm::GuestMemory& memory)
    : memory_(memory),
      pages_(std::make_unique<std::atomic<DecodedPage*>[]>(vm::kPageCount)),
      gens_(std::make_unique<std::atomic<u32>[]>(vm::kPageCount)) {
    // Frees of guest memory (sys_memory_free, PRX unload) drop the translated pages.
    memory_.SetInvalidateHook(
        [](void* self, u32 addr, u64 size) { static_cast<PpuCodeCache*>(self)->Invalidate(addr, size); }, this);
}

PpuCodeCache::~PpuCodeCache() {
    memory_.SetInvalidateHook(nullptr, nullptr);
    for (u32 p = 0; p < vm::kPageCount; ++p) delete pages_[p].load(std::memory_order_relaxed);
}

DecodedPage* PpuCodeCache::Decode(u32 page) noexcept {
    const u32 addr = page << vm::kPageShift;
    if (!(memory_.Flags(addr) & vm::kPageReadable)) return nullptr;
    // From here on, guest stores to this page call Invalidate(), which bumps gens_[page].
    memory_.MarkCode(addr, vm::kPageSize);
    const auto& handlers = PpuHandlerTable();
    const u8* src = memory_.base() + addr;
    for (;;) {
        const u32 generation = gens_[page].load(std::memory_order_acquire);
        auto decoded = std::make_unique<DecodedPage>();
        for (u32 i = 0; i < 1024; ++i) {
            // Relaxed atomic load: another PPU thread may be storing to this page right now.
            u32 raw = std::atomic_ref<u32>(*reinterpret_cast<u32*>(const_cast<u8*>(src) + i * 4))
                          .load(std::memory_order_relaxed);
            raw = ByteSwap(raw);
            const PpuInstr id = ::cell::ppu::Decode(raw);  // the decoder, not this method
            decoded->raw[i] = raw;
            decoded->id[i] = id;
            decoded->handler[i] = handlers[static_cast<std::size_t>(id)];
        }
        DecodedPage* expected = nullptr;
        DecodedPage* fresh = decoded.get();
        if (!pages_[page].compare_exchange_strong(expected, fresh, std::memory_order_acq_rel))
            return expected;  // another thread published first; ours is discarded
        decoded.release();
        // A store between our copy and the publish found the slot empty and retired nothing:
        // its generation bump tells us. Take our (possibly stale) page back and decode again.
        if (gens_[page].load(std::memory_order_acquire) == generation) return fresh;
        DecodedPage* mine = fresh;
        if (pages_[page].compare_exchange_strong(mine, nullptr, std::memory_order_acq_rel)) {
            std::lock_guard lock(retire_mutex_);
            retired_.emplace_back(fresh);
        }
    }
}

void PpuCodeCache::Invalidate(u32 addr, u64 size) noexcept {
    if (!size) return;
    const u64 first = addr >> vm::kPageShift;
    const u64 last = (u64{addr} + size - 1) >> vm::kPageShift;
    for (u64 p = first; p <= last && p < vm::kPageCount; ++p) {
        gens_[p].fetch_add(1, std::memory_order_acq_rel);  // before the swap: see Decode()
        if (DecodedPage* old = pages_[p].exchange(nullptr, std::memory_order_acq_rel)) {
            std::lock_guard lock(retire_mutex_);
            retired_.emplace_back(old);
        }
    }
}

std::size_t PpuCodeCache::retired_pages() const {
    std::lock_guard lock(retire_mutex_);
    return retired_.size();
}

}  // namespace cell::ppu
