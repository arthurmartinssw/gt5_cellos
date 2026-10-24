// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — ppu/ppu_decoder.h
//
// Instruction word -> PpuInstr. Table-driven, two levels, no branches on the hot path:
//
//   level 1: primary opcode (bits 0-5), 64 entries. Most opcodes resolve here.
//   level 2: for the six "extended" primaries, a secondary table indexed by that form's
//            extended opcode field:
//              4  VMX   VA-form xo (bits 26-31) when bit 26 is set, else VX/VC xo (bits 21-31)
//              19 XL    xo bits 21-30
//              30 MD    xo bits 27-29 (MD) / 27-30 (MDS)
//              31 X/XO  xo bits 21-30; XO-forms are entered twice (OE=0 and OE=1)
//              58/62 DS xo bits 30-31
//              59/63 A  xo bits 26-30 first, then (63 only) X-form xo bits 21-30
//
// The decoder knows nothing about execution: the interpreter maps PpuInstr to handlers,
// the JIT (phase 2b) will map it to emitters, and both stay in sync through
// PPU_INSTRUCTION_LIST.
#pragma once

#include <string>

#include "ppu/ppu_instructions.h"

namespace cell::ppu {

[[nodiscard]] PpuInstr Decode(u32 raw) noexcept;

// "primary 31, xo 0x153" style description of an undecodable word, for error reports.
[[nodiscard]] std::string DescribeEncoding(u32 raw);

}  // namespace cell::ppu
