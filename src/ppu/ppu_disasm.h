// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — ppu/ppu_disasm.h
//
// PPU disassembler for crash reports and traces. Built on the same Decode() as the
// interpreter (and later the JIT), so what it prints is what was executed. Output follows
// GNU objdump conventions ("lbz r0,0(r9)", simplified mnemonics like li/mr/blr/beq).
#pragma once

#include <optional>
#include <string>

#include "common/types.h"
#include "ppu/ppu_context.h"

namespace cell::ppu {

[[nodiscard]] std::string Disassemble(u32 pc, u32 raw);

// For a load/store: how its effective address was formed from the thread's registers,
// e.g. "EA = r9 (0x0) + 0 = 0x00000000". nullopt for non-memory instructions.
[[nodiscard]] std::optional<std::string> ExplainEffectiveAddress(u32 raw, const PpuThreadContext& ctx);

}  // namespace cell::ppu
