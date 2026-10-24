// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/spu_asm.h
//
// Minimal SPU assembler for tests: encodes any instruction of SPU_INSTRUCTION_LIST by form.
// Known real encodings are checked in spu_isa_tests so the table cannot drift silently.
#pragma once

#include <vector>

#include "spu/spu_decoder.h"

namespace sa {

using cell::u32;
using cell::spu::SpuOp;

inline u32 Op(SpuOp op) { return cell::spu::Info(op).opcode; }

inline u32 RR(SpuOp op, u32 rt, u32 ra, u32 rb) { return Op(op) << 21 | (rb & 0x7F) << 14 | (ra & 0x7F) << 7 | (rt & 0x7F); }
inline u32 RRR(SpuOp op, u32 rt, u32 ra, u32 rb, u32 rc) {
    return Op(op) << 28 | (rt & 0x7F) << 21 | (rb & 0x7F) << 14 | (ra & 0x7F) << 7 | (rc & 0x7F);
}
inline u32 RI7(SpuOp op, u32 rt, u32 ra, int i7) { return Op(op) << 21 | (static_cast<u32>(i7) & 0x7F) << 14 | (ra & 0x7F) << 7 | (rt & 0x7F); }
inline u32 RI8(SpuOp op, u32 rt, u32 ra, u32 i8) { return Op(op) << 22 | (i8 & 0xFF) << 14 | (ra & 0x7F) << 7 | (rt & 0x7F); }
inline u32 RI10(SpuOp op, u32 rt, u32 ra, int i10) {
    return Op(op) << 24 | (static_cast<u32>(i10) & 0x3FF) << 14 | (ra & 0x7F) << 7 | (rt & 0x7F);
}
inline u32 RI16(SpuOp op, u32 rt, int i16) { return Op(op) << 23 | (static_cast<u32>(i16) & 0xFFFF) << 7 | (rt & 0x7F); }
inline u32 RI18(SpuOp op, u32 rt, u32 i18) { return Op(op) << 25 | (i18 & 0x3FFFF) << 7 | (rt & 0x7F); }

inline u32 Stop(u32 code) { return code & 0x3FFF; }
inline u32 Rdch(u32 rt, u32 ch) { return RR(SpuOp::RDCH, rt, ch, 0); }
inline u32 Wrch(u32 ch, u32 rt) { return RR(SpuOp::WRCH, rt, ch, 0); }
inline u32 Il(u32 rt, int v) { return RI16(SpuOp::IL, rt, v); }
inline u32 Ai(u32 rt, u32 ra, int v) { return RI10(SpuOp::AI, rt, ra, v); }
// lqd/stqd take the byte offset (multiple of 16)
inline u32 Lqd(u32 rt, int offset, u32 ra) { return RI10(SpuOp::LQD, rt, ra, offset / 16); }
inline u32 Stqd(u32 rt, int offset, u32 ra) { return RI10(SpuOp::STQD, rt, ra, offset / 16); }
// relative branches take the word offset from the branch itself
inline u32 Br(int words) { return RI16(SpuOp::BR, 0, words); }
inline u32 Brsl(u32 rt, int words) { return RI16(SpuOp::BRSL, rt, words); }
inline u32 Brnz(u32 rt, int words) { return RI16(SpuOp::BRNZ, rt, words); }
inline u32 Bi(u32 ra) { return RR(SpuOp::BI, 0, ra, 0); }
// rt = 32-bit constant in every word
inline std::vector<u32> Load32(u32 rt, u32 value) {
    return {RI16(SpuOp::ILHU, rt, static_cast<int>(value >> 16)), RI16(SpuOp::IOHL, rt, static_cast<int>(value & 0xFFFF))};
}
// sys_spu_thread_exit(status in rt's preferred word)
inline std::vector<u32> Exit(u32 rt) { return {Wrch(28, rt), Stop(0x102)}; }

}  // namespace sa
