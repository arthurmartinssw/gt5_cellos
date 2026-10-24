// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — spu/spu_instructions.h
//
// The one list of SPU instructions (Synergistic Processor Unit ISA v1.2). Everything else —
// decoder table, interpreter dispatch, disassembler, test assembler — is generated from it.
//
// Encodings (bit 0 = most significant):
//   RR   op[0:10]  RB[11:17] RA[18:24] RT[25:31]
//   RRR  op[0:3]   RT[4:10]  RB[11:17] RA[18:24] RC[25:31]     (RT is the *destination*)
//   RI7  op[0:10]  I7[11:17] RA[18:24] RT[25:31]
//   RI8  op[0:9]   I8[10:17] RA[18:24] RT[25:31]
//   RI10 op[0:7]   I10[8:17] RA[18:24] RT[25:31]
//   RI16 op[0:8]   I16[9:24]           RT[25:31]
//   RI18 op[0:6]   I18[7:24]           RT[25:31]
// `bits` is the opcode width; the decoder indexes a 2048-entry table by the top 11 bits and
// fills every slot a shorter opcode covers. Values cross-checked against known encodings
// (e.g. `ai $1,$1,-32` = 0x1cf80081, `stqd $0,16($1)` = 0x24004080, `nop` = 0x40200000,
// `bi $0` = 0x35000000, `il $3,0` = 0x40800003).
#pragma once

#include "common/types.h"

namespace cell::spu {

enum class SpuForm : u8 { RR, RRR, RI7, RI8, RI10, RI16, RI18 };

// X(NAME, mnemonic, form, opcode, opcode bits)
// clang-format off
#define SPU_INSTRUCTION_LIST(X) \
    /* control */ \
    X(STOP, "stop", RR, 0x000, 11) X(LNOP, "lnop", RR, 0x001, 11) X(SYNC, "sync", RR, 0x002, 11) \
    X(DSYNC, "dsync", RR, 0x003, 11) X(MFSPR, "mfspr", RR, 0x00C, 11) X(RDCH, "rdch", RR, 0x00D, 11) \
    X(RCHCNT, "rchcnt", RR, 0x00F, 11) X(MTSPR, "mtspr", RR, 0x10C, 11) X(WRCH, "wrch", RR, 0x10D, 11) \
    X(STOPD, "stopd", RR, 0x140, 11) X(NOP, "nop", RR, 0x201, 11) \
    /* integer / logical RR */ \
    X(SF, "sf", RR, 0x040, 11) X(OR, "or", RR, 0x041, 11) X(BG, "bg", RR, 0x042, 11) X(SFH, "sfh", RR, 0x048, 11) \
    X(NOR, "nor", RR, 0x049, 11) X(ABSDB, "absdb", RR, 0x053, 11) X(A, "a", RR, 0x0C0, 11) X(AND, "and", RR, 0x0C1, 11) \
    X(CG, "cg", RR, 0x0C2, 11) X(AH, "ah", RR, 0x0C8, 11) X(NAND, "nand", RR, 0x0C9, 11) X(AVGB, "avgb", RR, 0x0D3, 11) \
    X(CGT, "cgt", RR, 0x240, 11) X(XOR, "xor", RR, 0x241, 11) X(CGTH, "cgth", RR, 0x248, 11) X(EQV, "eqv", RR, 0x249, 11) \
    X(CGTB, "cgtb", RR, 0x250, 11) X(SUMB, "sumb", RR, 0x253, 11) X(HGT, "hgt", RR, 0x258, 11) \
    X(CLZ, "clz", RR, 0x2A5, 11) X(XSWD, "xswd", RR, 0x2A6, 11) X(XSHW, "xshw", RR, 0x2AE, 11) \
    X(CNTB, "cntb", RR, 0x2B4, 11) X(XSBH, "xsbh", RR, 0x2B6, 11) X(CLGT, "clgt", RR, 0x2C0, 11) \
    X(ANDC, "andc", RR, 0x2C1, 11) X(CLGTH, "clgth", RR, 0x2C8, 11) X(ORC, "orc", RR, 0x2C9, 11) \
    X(CLGTB, "clgtb", RR, 0x2D0, 11) X(HLGT, "hlgt", RR, 0x2D8, 11) X(ADDX, "addx", RR, 0x340, 11) \
    X(SFX, "sfx", RR, 0x341, 11) X(CGX, "cgx", RR, 0x342, 11) X(BGX, "bgx", RR, 0x343, 11) \
    X(MPYHHA, "mpyhha", RR, 0x346, 11) X(MPYHHAU, "mpyhhau", RR, 0x34E, 11) X(CEQ, "ceq", RR, 0x3C0, 11) \
    X(MPY, "mpy", RR, 0x3C4, 11) X(MPYH, "mpyh", RR, 0x3C5, 11) X(MPYHH, "mpyhh", RR, 0x3C6, 11) \
    X(MPYS, "mpys", RR, 0x3C7, 11) X(CEQH, "ceqh", RR, 0x3C8, 11) X(MPYU, "mpyu", RR, 0x3CC, 11) \
    X(MPYHHU, "mpyhhu", RR, 0x3CE, 11) X(CEQB, "ceqb", RR, 0x3D0, 11) X(HEQ, "heq", RR, 0x3D8, 11) \
    /* shifts / rotates */ \
    X(ROT, "rot", RR, 0x058, 11) X(ROTM, "rotm", RR, 0x059, 11) X(ROTMA, "rotma", RR, 0x05A, 11) \
    X(SHL, "shl", RR, 0x05B, 11) X(ROTH, "roth", RR, 0x05C, 11) X(ROTHM, "rothm", RR, 0x05D, 11) \
    X(ROTMAH, "rotmah", RR, 0x05E, 11) X(SHLH, "shlh", RR, 0x05F, 11) \
    X(ROTI, "roti", RI7, 0x078, 11) X(ROTMI, "rotmi", RI7, 0x079, 11) X(ROTMAI, "rotmai", RI7, 0x07A, 11) \
    X(SHLI, "shli", RI7, 0x07B, 11) X(ROTHI, "rothi", RI7, 0x07C, 11) X(ROTHMI, "rothmi", RI7, 0x07D, 11) \
    X(ROTMAHI, "rotmahi", RI7, 0x07E, 11) X(SHLHI, "shlhi", RI7, 0x07F, 11) \
    X(ROTQBYBI, "rotqbybi", RR, 0x1CC, 11) X(ROTQMBYBI, "rotqmbybi", RR, 0x1CD, 11) \
    X(SHLQBYBI, "shlqbybi", RR, 0x1CF, 11) X(ROTQBI, "rotqbi", RR, 0x1D8, 11) X(ROTQMBI, "rotqmbi", RR, 0x1D9, 11) \
    X(SHLQBI, "shlqbi", RR, 0x1DB, 11) X(ROTQBY, "rotqby", RR, 0x1DC, 11) X(ROTQMBY, "rotqmby", RR, 0x1DD, 11) \
    X(SHLQBY, "shlqby", RR, 0x1DF, 11) X(ROTQBII, "rotqbii", RI7, 0x1F8, 11) \
    X(ROTQMBII, "rotqmbii", RI7, 0x1F9, 11) X(SHLQBII, "shlqbii", RI7, 0x1FB, 11) \
    X(ROTQBYI, "rotqbyi", RI7, 0x1FC, 11) X(ROTQMBYI, "rotqmbyi", RI7, 0x1FD, 11) \
    X(SHLQBYI, "shlqbyi", RI7, 0x1FF, 11) \
    /* branches, hints */ \
    X(BIZ, "biz", RR, 0x128, 11) X(BINZ, "binz", RR, 0x129, 11) X(BIHZ, "bihz", RR, 0x12A, 11) \
    X(BIHNZ, "bihnz", RR, 0x12B, 11) X(BI, "bi", RR, 0x1A8, 11) X(BISL, "bisl", RR, 0x1A9, 11) \
    X(IRET, "iret", RR, 0x1AA, 11) X(BISLED, "bisled", RR, 0x1AB, 11) X(HBR, "hbr", RR, 0x1AC, 11) \
    X(BRZ, "brz", RI16, 0x040, 9) X(BRNZ, "brnz", RI16, 0x042, 9) X(BRHZ, "brhz", RI16, 0x044, 9) \
    X(BRHNZ, "brhnz", RI16, 0x046, 9) X(BRA, "bra", RI16, 0x060, 9) X(BRASL, "brasl", RI16, 0x062, 9) \
    X(BR, "br", RI16, 0x064, 9) X(BRSL, "brsl", RI16, 0x066, 9) \
    X(HBRA, "hbra", RI18, 0x08, 7) X(HBRR, "hbrr", RI18, 0x09, 7) \
    /* gather / select masks, misc */ \
    X(GB, "gb", RR, 0x1B0, 11) X(GBH, "gbh", RR, 0x1B1, 11) X(GBB, "gbb", RR, 0x1B2, 11) \
    X(FSM, "fsm", RR, 0x1B4, 11) X(FSMH, "fsmh", RR, 0x1B5, 11) X(FSMB, "fsmb", RR, 0x1B6, 11) \
    X(ORX, "orx", RR, 0x1F0, 11) X(FSMBI, "fsmbi", RI16, 0x065, 9) \
    /* loads / stores, insertion controls */ \
    X(LQX, "lqx", RR, 0x1C4, 11) X(STQX, "stqx", RR, 0x144, 11) X(LQD, "lqd", RI10, 0x34, 8) \
    X(STQD, "stqd", RI10, 0x24, 8) X(LQA, "lqa", RI16, 0x061, 9) X(LQR, "lqr", RI16, 0x067, 9) \
    X(STQA, "stqa", RI16, 0x041, 9) X(STQR, "stqr", RI16, 0x047, 9) \
    X(CBX, "cbx", RR, 0x1D4, 11) X(CHX, "chx", RR, 0x1D5, 11) X(CWX, "cwx", RR, 0x1D6, 11) X(CDX, "cdx", RR, 0x1D7, 11) \
    X(CBD, "cbd", RI7, 0x1F4, 11) X(CHD, "chd", RI7, 0x1F5, 11) X(CWD, "cwd", RI7, 0x1F6, 11) X(CDD, "cdd", RI7, 0x1F7, 11) \
    /* immediates */ \
    X(IL, "il", RI16, 0x081, 9) X(ILHU, "ilhu", RI16, 0x082, 9) X(ILH, "ilh", RI16, 0x083, 9) \
    X(IOHL, "iohl", RI16, 0x0C1, 9) X(ILA, "ila", RI18, 0x21, 7) \
    X(ORI, "ori", RI10, 0x04, 8) X(ORHI, "orhi", RI10, 0x05, 8) X(ORBI, "orbi", RI10, 0x06, 8) \
    X(SFI, "sfi", RI10, 0x0C, 8) X(SFHI, "sfhi", RI10, 0x0D, 8) X(ANDI, "andi", RI10, 0x14, 8) \
    X(ANDHI, "andhi", RI10, 0x15, 8) X(ANDBI, "andbi", RI10, 0x16, 8) X(AI, "ai", RI10, 0x1C, 8) \
    X(AHI, "ahi", RI10, 0x1D, 8) X(XORI, "xori", RI10, 0x44, 8) X(XORHI, "xorhi", RI10, 0x45, 8) \
    X(XORBI, "xorbi", RI10, 0x46, 8) X(CGTI, "cgti", RI10, 0x4C, 8) X(CGTHI, "cgthi", RI10, 0x4D, 8) \
    X(CGTBI, "cgtbi", RI10, 0x4E, 8) X(HGTI, "hgti", RI10, 0x4F, 8) X(CLGTI, "clgti", RI10, 0x5C, 8) \
    X(CLGTHI, "clgthi", RI10, 0x5D, 8) X(CLGTBI, "clgtbi", RI10, 0x5E, 8) X(HLGTI, "hlgti", RI10, 0x5F, 8) \
    X(MPYI, "mpyi", RI10, 0x74, 8) X(MPYUI, "mpyui", RI10, 0x75, 8) X(CEQI, "ceqi", RI10, 0x7C, 8) \
    X(CEQHI, "ceqhi", RI10, 0x7D, 8) X(CEQBI, "ceqbi", RI10, 0x7E, 8) X(HEQI, "heqi", RI10, 0x7F, 8) \
    /* RRR */ \
    X(SELB, "selb", RRR, 0x8, 4) X(SHUFB, "shufb", RRR, 0xB, 4) X(MPYA, "mpya", RRR, 0xC, 4) \
    X(FNMS, "fnms", RRR, 0xD, 4) X(FMA, "fma", RRR, 0xE, 4) X(FMS, "fms", RRR, 0xF, 4) \
    /* single precision */ \
    X(FCGT, "fcgt", RR, 0x2C2, 11) X(FA, "fa", RR, 0x2C4, 11) X(FS, "fs", RR, 0x2C5, 11) X(FM, "fm", RR, 0x2C6, 11) \
    X(FCMGT, "fcmgt", RR, 0x2CA, 11) X(FCEQ, "fceq", RR, 0x3C2, 11) X(FCMEQ, "fcmeq", RR, 0x3CA, 11) \
    X(FREST, "frest", RR, 0x1B8, 11) X(FRSQEST, "frsqest", RR, 0x1B9, 11) X(FI, "fi", RR, 0x3D4, 11) \
    X(CFLTS, "cflts", RI8, 0x1D8, 10) X(CFLTU, "cfltu", RI8, 0x1D9, 10) X(CSFLT, "csflt", RI8, 0x1DA, 10) \
    X(CUFLT, "cuflt", RI8, 0x1DB, 10) X(FSCRRD, "fscrrd", RR, 0x398, 11) X(FSCRWR, "fscrwr", RR, 0x3BA, 11) \
    /* double precision */ \
    X(DFCGT, "dfcgt", RR, 0x2C3, 11) X(DFCMGT, "dfcmgt", RR, 0x2CB, 11) X(DFA, "dfa", RR, 0x2CC, 11) \
    X(DFS, "dfs", RR, 0x2CD, 11) X(DFM, "dfm", RR, 0x2CE, 11) X(DFMA, "dfma", RR, 0x35C, 11) \
    X(DFMS, "dfms", RR, 0x35D, 11) X(DFNMS, "dfnms", RR, 0x35E, 11) X(DFNMA, "dfnma", RR, 0x35F, 11) \
    X(DFCEQ, "dfceq", RR, 0x3C3, 11) X(DFCMEQ, "dfcmeq", RR, 0x3CB, 11) X(DFTSV, "dftsv", RI7, 0x3BF, 11) \
    X(FESD, "fesd", RR, 0x3B8, 11) X(FRDS, "frds", RR, 0x3B9, 11)
// clang-format on

enum class SpuOp : u16 {
    Invalid,
#define SPU_X_ENUM(name, mnem, form, opcode, bits) name,
    SPU_INSTRUCTION_LIST(SPU_X_ENUM)
#undef SPU_X_ENUM
    Count
};

struct SpuOpInfo {
    const char* mnemonic;
    SpuForm form;
    u16 opcode;
    u8 bits;
};

// Field extraction (instruction word as fetched, host order).
[[nodiscard]] constexpr u32 FieldRT(u32 i) noexcept { return i & 0x7F; }          // RR/RI* target, RRR source RC
[[nodiscard]] constexpr u32 FieldRA(u32 i) noexcept { return (i >> 7) & 0x7F; }
[[nodiscard]] constexpr u32 FieldRB(u32 i) noexcept { return (i >> 14) & 0x7F; }
[[nodiscard]] constexpr u32 FieldRT4(u32 i) noexcept { return (i >> 21) & 0x7F; }  // RRR target
[[nodiscard]] constexpr s32 FieldI7(u32 i) noexcept { return static_cast<s32>(((i >> 14) & 0x7F) ^ 0x40) - 0x40; }
[[nodiscard]] constexpr u32 FieldI8(u32 i) noexcept { return (i >> 14) & 0xFF; }
[[nodiscard]] constexpr s32 FieldI10(u32 i) noexcept { return static_cast<s32>(((i >> 14) & 0x3FF) ^ 0x200) - 0x200; }
[[nodiscard]] constexpr u32 FieldI16(u32 i) noexcept { return (i >> 7) & 0xFFFF; }
[[nodiscard]] constexpr s32 FieldS16(u32 i) noexcept { return static_cast<s16>(FieldI16(i)); }
[[nodiscard]] constexpr u32 FieldI18(u32 i) noexcept { return (i >> 7) & 0x3FFFF; }

}  // namespace cell::spu
