// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/guest_format.h
//
// printf-family formatting with arguments taken from a PPU call: integer/pointer/double
// varargs come from r3..r10, then from the caller's parameter save area at r1 + 48 + 8*i
// (PPC64 ELF ABI). PS3 PPU types: int 32-bit, long 64-bit, pointers 32-bit.
#pragma once

#include <string>

#include "common/types.h"
#include "memory/guest_memory.h"
#include "ppu/ppu_context.h"

namespace cell::hle {

// `first_vararg`: index of the first variadic argument (printf: 1, sprintf: 2, snprintf: 3).
[[nodiscard]] std::string FormatGuest(const vm::GuestMemory& memory, const ppu::PpuThreadContext& ctx, u32 format_ea,
                                      u32 first_vararg);
// v*printf: arguments come from a va_list. Under the PS3 PPU ABI (PPC64 ELF) a va_list is a
// pointer into the parameter save area: one 8-byte slot per argument, doubles as raw bits.
[[nodiscard]] std::string FormatGuestVa(const vm::GuestMemory& memory, u32 format_ea, u32 va_list_ea);

}  // namespace cell::hle
