// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — memory/guest_copy.h
//
// Copies between guest memory regions that other host threads may access at the same time
// (MFC DMA between main memory and local stores, PPU syscalls touching a running SPU's LS).
// Naturally aligned 1/2/4/8-byte units are moved with relaxed std::atomic_ref, so concurrent
// guest accesses are well-defined in C++ (and invisible to ThreadSanitizer) while compiling
// to plain loads/stores on x86-64 and ARM64. Element-wise atomicity is all DMA guarantees
// on real hardware too; no ordering is implied.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "common/types.h"

namespace cell::vm {

namespace detail {
template <typename T>
inline void CopyUnit(u8* dst, const u8* src) noexcept {
    const T v = std::atomic_ref<T>(*reinterpret_cast<T*>(const_cast<u8*>(src))).load(std::memory_order_relaxed);
    std::atomic_ref<T>(*reinterpret_cast<T*>(dst)).store(v, std::memory_order_relaxed);
}
}  // namespace detail

inline void GuestCopy(u8* dst, const u8* src, std::size_t size) noexcept {
    const auto d = reinterpret_cast<std::uintptr_t>(dst), s = reinterpret_cast<std::uintptr_t>(src);
    if (((d | s | size) & 7) == 0) {
        for (std::size_t i = 0; i < size; i += 8) detail::CopyUnit<u64>(dst + i, src + i);
    } else if (((d | s | size) & 3) == 0) {
        for (std::size_t i = 0; i < size; i += 4) detail::CopyUnit<u32>(dst + i, src + i);
    } else if (((d | s | size) & 1) == 0) {
        for (std::size_t i = 0; i < size; i += 2) detail::CopyUnit<u16>(dst + i, src + i);
    } else {
        for (std::size_t i = 0; i < size; ++i) detail::CopyUnit<u8>(dst + i, src + i);
    }
}

}  // namespace cell::vm
