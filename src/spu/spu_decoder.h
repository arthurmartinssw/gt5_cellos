// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — spu/spu_decoder.h
//
// SPU decoding: one 2048-entry table indexed by the top 11 bits of the instruction, generated
// from SPU_INSTRUCTION_LIST (a shorter opcode fills every slot it covers). Shared by the
// interpreter, the disassembler and — next — the SPU -> x86-64 translator.
#pragma once

#include <string>

#include "spu/spu_instructions.h"

namespace cell::spu {

[[nodiscard]] SpuOp Decode(u32 instruction) noexcept;
[[nodiscard]] const SpuOpInfo& Info(SpuOp op) noexcept;
// Two opcodes claiming the same table slot (a typo in the list); empty when consistent.
[[nodiscard]] std::string DecoderConflicts();

// "ai $1,$1,-32", "brsl $0,0x01230", "lqd $3,16($1)" ... `pc` resolves relative branches.
[[nodiscard]] std::string Disassemble(u32 pc, u32 instruction);

}  // namespace cell::spu
