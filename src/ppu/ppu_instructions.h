// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — ppu/ppu_instructions.h
//
// The one list of PPU instructions this project knows. Everything else is generated from it:
//   * enum class PpuInstr      — what the decoder returns (shared by interpreter and JIT),
//   * PpuInstrName()           — mnemonics for traces and "unimplemented" reports,
//   * the interpreter's handler table (one function per entry, checked at compile time).
// Adding an instruction = one line here + one encoding in ppu_decoder.cpp + one handler.
#pragma once

#include "common/types.h"

namespace cell::ppu {

// clang-format off
#define PPU_INSTRUCTION_LIST(X) \
    X(UNK, "<unknown>") X(HLE, "hle") \
    /* primary opcodes */ \
    X(TDI, "tdi") X(TWI, "twi") X(MULLI, "mulli") X(SUBFIC, "subfic") X(CMPLI, "cmpli") X(CMPI, "cmpi") \
    X(ADDIC, "addic") X(ADDIC_, "addic.") X(ADDI, "addi") X(ADDIS, "addis") X(BC, "bc") X(SC, "sc") X(B, "b") \
    X(RLWIMI, "rlwimi") X(RLWINM, "rlwinm") X(RLWNM, "rlwnm") X(ORI, "ori") X(ORIS, "oris") X(XORI, "xori") \
    X(XORIS, "xoris") X(ANDI_, "andi.") X(ANDIS_, "andis.") \
    X(LWZ, "lwz") X(LWZU, "lwzu") X(LBZ, "lbz") X(LBZU, "lbzu") X(STW, "stw") X(STWU, "stwu") X(STB, "stb") \
    X(STBU, "stbu") X(LHZ, "lhz") X(LHZU, "lhzu") X(LHA, "lha") X(LHAU, "lhau") X(STH, "sth") X(STHU, "sthu") \
    X(LMW, "lmw") X(STMW, "stmw") X(LFS, "lfs") X(LFSU, "lfsu") X(LFD, "lfd") X(LFDU, "lfdu") X(STFS, "stfs") \
    X(STFSU, "stfsu") X(STFD, "stfd") X(STFDU, "stfdu") X(LD, "ld") X(LDU, "ldu") X(LWA, "lwa") X(STD, "std") \
    X(STDU, "stdu") \
    /* opcode 19 */ \
    X(MCRF, "mcrf") X(BCLR, "bclr") X(CRNOR, "crnor") X(CRANDC, "crandc") X(ISYNC, "isync") X(CRXOR, "crxor") \
    X(CRNAND, "crnand") X(CRAND, "crand") X(CREQV, "creqv") X(CRORC, "crorc") X(CROR, "cror") X(BCCTR, "bcctr") \
    /* opcode 30 */ \
    X(RLDICL, "rldicl") X(RLDICR, "rldicr") X(RLDIC, "rldic") X(RLDIMI, "rldimi") X(RLDCL, "rldcl") X(RLDCR, "rldcr") \
    /* opcode 31: integer */ \
    X(CMP, "cmp") X(TW, "tw") X(SUBFC, "subfc") X(MULHDU, "mulhdu") X(ADDC, "addc") X(MULHWU, "mulhwu") \
    X(MFCR, "mfcr") X(SLW, "slw") X(CNTLZW, "cntlzw") X(SLD, "sld") X(AND, "and") X(CMPL, "cmpl") X(SUBF, "subf") \
    X(CNTLZD, "cntlzd") X(ANDC, "andc") X(TD, "td") X(MULHD, "mulhd") X(MULHW, "mulhw") X(NEG, "neg") X(NOR, "nor") \
    X(SUBFE, "subfe") X(ADDE, "adde") X(MTCRF, "mtcrf") X(SUBFZE, "subfze") X(ADDZE, "addze") X(SUBFME, "subfme") \
    X(MULLD, "mulld") X(ADDME, "addme") X(MULLW, "mullw") X(ADD, "add") X(EQV, "eqv") X(XOR, "xor") \
    X(MFSPR, "mfspr") X(MFTB, "mftb") X(ORC, "orc") X(OR, "or") X(DIVDU, "divdu") X(DIVWU, "divwu") \
    X(MTSPR, "mtspr") X(NAND, "nand") X(DIVD, "divd") X(DIVW, "divw") X(SRW, "srw") X(SRD, "srd") X(SRAW, "sraw") \
    X(SRAD, "srad") X(SRAWI, "srawi") X(SRADI, "sradi") X(EXTSH, "extsh") X(EXTSB, "extsb") X(EXTSW, "extsw") \
    /* opcode 31: loads / stores / cache / sync */ \
    X(LWARX, "lwarx") X(LDX, "ldx") X(LWZX, "lwzx") X(LDUX, "ldux") X(LWZUX, "lwzux") X(LDARX, "ldarx") \
    X(LBZX, "lbzx") X(LBZUX, "lbzux") X(STDX, "stdx") X(STWCX, "stwcx.") X(STWX, "stwx") X(STDUX, "stdux") \
    X(STWUX, "stwux") X(STDCX, "stdcx.") X(STBX, "stbx") X(STBUX, "stbux") X(LHZX, "lhzx") X(LHZUX, "lhzux") \
    X(LWAX, "lwax") X(LHAX, "lhax") X(LWAUX, "lwaux") X(LHAUX, "lhaux") X(STHX, "sthx") X(STHUX, "sthux") \
    X(LWBRX, "lwbrx") X(STWBRX, "stwbrx") X(LHBRX, "lhbrx") X(STHBRX, "sthbrx") \
    X(LFSX, "lfsx") X(LFSUX, "lfsux") X(LFDX, "lfdx") X(LFDUX, "lfdux") X(STFSX, "stfsx") X(STFSUX, "stfsux") \
    X(STFDX, "stfdx") X(STFDUX, "stfdux") X(STFIWX, "stfiwx") \
    X(DCBST, "dcbst") X(DCBF, "dcbf") X(DCBTST, "dcbtst") X(DCBT, "dcbt") X(DCBZ, "dcbz") X(ICBI, "icbi") \
    X(SYNC, "sync") X(EIEIO, "eieio") X(DST, "dst") X(DSTST, "dstst") X(DSS, "dss") \
    /* opcode 31: VMX memory (incl. Cell lvlx/lvrx family) */ \
    X(LVSL, "lvsl") X(LVSR, "lvsr") X(LVEBX, "lvebx") X(LVEHX, "lvehx") X(LVEWX, "lvewx") X(LVX, "lvx") \
    X(STVEBX, "stvebx") X(STVEHX, "stvehx") X(STVEWX, "stvewx") X(STVX, "stvx") \
    X(LVLX, "lvlx") X(LVRX, "lvrx") X(STVLX, "stvlx") X(STVRX, "stvrx") \
    /* opcode 59 */ \
    X(FDIVS, "fdivs") X(FSUBS, "fsubs") X(FADDS, "fadds") X(FSQRTS, "fsqrts") X(FRES, "fres") X(FMULS, "fmuls") \
    X(FMSUBS, "fmsubs") X(FMADDS, "fmadds") X(FNMSUBS, "fnmsubs") X(FNMADDS, "fnmadds") \
    /* opcode 63 */ \
    X(FDIV, "fdiv") X(FSUB, "fsub") X(FADD, "fadd") X(FSQRT, "fsqrt") X(FSEL, "fsel") X(FMUL, "fmul") \
    X(FRSQRTE, "frsqrte") X(FMSUB, "fmsub") X(FMADD, "fmadd") X(FNMSUB, "fnmsub") X(FNMADD, "fnmadd") \
    X(FCMPU, "fcmpu") X(FRSP, "frsp") X(FCTIW, "fctiw") X(FCTIWZ, "fctiwz") X(FCMPO, "fcmpo") X(MTFSB1, "mtfsb1") \
    X(FNEG, "fneg") X(MCRFS, "mcrfs") X(MTFSB0, "mtfsb0") X(FMR, "fmr") X(MTFSFI, "mtfsfi") X(FNABS, "fnabs") \
    X(FABS, "fabs") X(MFFS, "mffs") X(MTFSF, "mtfsf") X(FCTID, "fctid") X(FCTIDZ, "fctidz") X(FCFID, "fcfid") \
    /* opcode 4: VMX */ \
    X(VSEL, "vsel") X(VPERM, "vperm") X(VSLDOI, "vsldoi") X(VMADDFP, "vmaddfp") X(VNMSUBFP, "vnmsubfp") \
    X(VADDUBM, "vaddubm") X(VADDUHM, "vadduhm") X(VADDUWM, "vadduwm") X(VADDFP, "vaddfp") \
    X(VSUBUBM, "vsububm") X(VSUBUHM, "vsubuhm") X(VSUBUWM, "vsubuwm") X(VSUBFP, "vsubfp") \
    X(VAND, "vand") X(VANDC, "vandc") X(VOR, "vor") X(VNOR, "vnor") X(VXOR, "vxor") \
    X(VSPLTB, "vspltb") X(VSPLTH, "vsplth") X(VSPLTW, "vspltw") \
    X(VSPLTISB, "vspltisb") X(VSPLTISH, "vspltish") X(VSPLTISW, "vspltisw") \
    X(VMRGHB, "vmrghb") X(VMRGHH, "vmrghh") X(VMRGHW, "vmrghw") X(VMRGLB, "vmrglb") X(VMRGLH, "vmrglh") \
    X(VMRGLW, "vmrglw") X(VSLW, "vslw") X(VSRW, "vsrw") X(VSRAW, "vsraw") X(VRLW, "vrlw") \
    X(VMAXFP, "vmaxfp") X(VMINFP, "vminfp") X(VMAXSW, "vmaxsw") X(VMINSW, "vminsw") X(VMAXUW, "vmaxuw") \
    X(VMINUW, "vminuw") X(VCFSX, "vcfsx") X(VCFUX, "vcfux") X(VCTSXS, "vctsxs") X(VCTUXS, "vctuxs") \
    X(VREFP, "vrefp") X(VRSQRTEFP, "vrsqrtefp") X(MFVSCR, "mfvscr") X(MTVSCR, "mtvscr") \
    X(VCMPEQUB, "vcmpequb") X(VCMPEQUH, "vcmpequh") X(VCMPEQUW, "vcmpequw") X(VCMPEQFP, "vcmpeqfp") \
    X(VCMPGEFP, "vcmpgefp") X(VCMPGTFP, "vcmpgtfp") X(VCMPGTUB, "vcmpgtub") X(VCMPGTUH, "vcmpgtuh") \
    X(VCMPGTUW, "vcmpgtuw") X(VCMPGTSB, "vcmpgtsb") X(VCMPGTSH, "vcmpgtsh") X(VCMPGTSW, "vcmpgtsw")
// clang-format on

enum class PpuInstr : u16 {
#define PPU_ENUM(id, name) id,
    PPU_INSTRUCTION_LIST(PPU_ENUM)
#undef PPU_ENUM
        Count
};

inline constexpr std::size_t kPpuInstrCount = static_cast<std::size_t>(PpuInstr::Count);

[[nodiscard]] inline const char* PpuInstrName(PpuInstr id) noexcept {
    static constexpr const char* kNames[] = {
#define PPU_NAME(id, name) name,
        PPU_INSTRUCTION_LIST(PPU_NAME)
#undef PPU_NAME
    };
    return kNames[static_cast<std::size_t>(id)];
}

}  // namespace cell::ppu
