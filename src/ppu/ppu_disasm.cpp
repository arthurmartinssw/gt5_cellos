// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — ppu/ppu_disasm.cpp
#include "ppu/ppu_disasm.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>

#include "hle/hle_registry.h"
#include "ppu/ppu_decoder.h"
#include "ppu/ppu_opcode.h"

namespace cell::ppu {

namespace {

using I = PpuInstr;

std::string Format(const char* format, ...) {
    char text[160];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

std::string Mnemonic(I id, Op op, bool has_rc, bool has_oe) {
    std::string m = PpuInstrName(id);
    if (has_oe && op.oe()) m += 'o';
    if (has_rc && op.rc()) m += '.';
    return m;
}

std::string Ra0(u32 ra) { return ra ? Format("r%u", ra) : std::string("0"); }

enum class Kind {
    Other, LoadStoreD, LoadStoreDS, FpLoadStoreD, LoadStoreX, FpLoadStoreX, VecLoadStoreX, ArithImm, LogicImm,
    CmpImm, Cmp, XoBinary, XoUnary, LogicX, LogicUnary, RotW, RotWReg, RotD, RotDReg, FpBinary, FpMul, FpFma,
    FpUnary, FpCmp, VecVa, VecVx,
};

Kind Classify(I id) {
    switch (id) {
    case I::LWZ: case I::LWZU: case I::LBZ: case I::LBZU: case I::STW: case I::STWU: case I::STB: case I::STBU:
    case I::LHZ: case I::LHZU: case I::LHA: case I::LHAU: case I::STH: case I::STHU: case I::LMW: case I::STMW:
        return Kind::LoadStoreD;
    case I::LD: case I::LDU: case I::LWA: case I::STD: case I::STDU:
        return Kind::LoadStoreDS;
    case I::LFS: case I::LFSU: case I::LFD: case I::LFDU: case I::STFS: case I::STFSU: case I::STFD: case I::STFDU:
        return Kind::FpLoadStoreD;
    case I::LWARX: case I::LDX: case I::LWZX: case I::LDUX: case I::LWZUX: case I::LDARX: case I::LBZX:
    case I::LBZUX: case I::STDX: case I::STWCX: case I::STWX: case I::STDUX: case I::STWUX: case I::STDCX:
    case I::STBX: case I::STBUX: case I::LHZX: case I::LHZUX: case I::LWAX: case I::LHAX: case I::LWAUX:
    case I::LHAUX: case I::STHX: case I::STHUX: case I::LWBRX: case I::STWBRX: case I::LHBRX: case I::STHBRX:
    case I::DCBST: case I::DCBF: case I::DCBTST: case I::DCBT: case I::DCBZ: case I::ICBI:
        return Kind::LoadStoreX;
    case I::LFSX: case I::LFSUX: case I::LFDX: case I::LFDUX: case I::STFSX: case I::STFSUX: case I::STFDX:
    case I::STFDUX: case I::STFIWX:
        return Kind::FpLoadStoreX;
    case I::LVSL: case I::LVSR: case I::LVEBX: case I::LVEHX: case I::LVEWX: case I::LVX: case I::STVEBX:
    case I::STVEHX: case I::STVEWX: case I::STVX: case I::LVLX: case I::LVRX: case I::STVLX: case I::STVRX:
        return Kind::VecLoadStoreX;
    case I::ADDI: case I::ADDIS: case I::ADDIC: case I::ADDIC_: case I::SUBFIC: case I::MULLI:
        return Kind::ArithImm;
    case I::ORI: case I::ORIS: case I::XORI: case I::XORIS: case I::ANDI_: case I::ANDIS_:
        return Kind::LogicImm;
    case I::CMPI: case I::CMPLI: return Kind::CmpImm;
    case I::CMP: case I::CMPL: return Kind::Cmp;
    case I::ADD: case I::ADDC: case I::ADDE: case I::SUBF: case I::SUBFC: case I::SUBFE: case I::MULLW:
    case I::MULLD: case I::MULHW: case I::MULHWU: case I::MULHD: case I::MULHDU: case I::DIVW: case I::DIVWU:
    case I::DIVD: case I::DIVDU:
        return Kind::XoBinary;
    case I::NEG: case I::ADDZE: case I::ADDME: case I::SUBFZE: case I::SUBFME: return Kind::XoUnary;
    case I::AND: case I::OR: case I::XOR: case I::NAND: case I::NOR: case I::EQV: case I::ANDC: case I::ORC:
    case I::SLW: case I::SRW: case I::SLD: case I::SRD: case I::SRAW: case I::SRAD:
        return Kind::LogicX;
    case I::EXTSB: case I::EXTSH: case I::EXTSW: case I::CNTLZW: case I::CNTLZD: return Kind::LogicUnary;
    case I::RLWINM: case I::RLWIMI: return Kind::RotW;
    case I::RLWNM: return Kind::RotWReg;
    case I::RLDICL: case I::RLDICR: case I::RLDIC: case I::RLDIMI: return Kind::RotD;
    case I::RLDCL: case I::RLDCR: return Kind::RotDReg;
    case I::FADD: case I::FSUB: case I::FDIV: case I::FADDS: case I::FSUBS: case I::FDIVS: return Kind::FpBinary;
    case I::FMUL: case I::FMULS: return Kind::FpMul;
    case I::FMADD: case I::FMSUB: case I::FNMADD: case I::FNMSUB: case I::FMADDS: case I::FMSUBS:
    case I::FNMADDS: case I::FNMSUBS: case I::FSEL:
        return Kind::FpFma;
    case I::FMR: case I::FNEG: case I::FABS: case I::FNABS: case I::FRSP: case I::FCTIW: case I::FCTIWZ:
    case I::FCTID: case I::FCTIDZ: case I::FCFID: case I::FSQRT: case I::FSQRTS: case I::FRES: case I::FRSQRTE:
        return Kind::FpUnary;
    case I::FCMPU: case I::FCMPO: return Kind::FpCmp;
    case I::VSEL: case I::VPERM: case I::VMADDFP: case I::VNMSUBFP: return Kind::VecVa;
    default:
        break;
    }
    const auto index = static_cast<std::size_t>(id);
    if (index >= static_cast<std::size_t>(I::VADDUBM) && index < kPpuInstrCount) return Kind::VecVx;
    return Kind::Other;
}

bool IsUpdateOrLoad(I id) {  // forms whose RA is also written (shown only for clarity)
    return id == I::LWZU || id == I::LBZU || id == I::STWU || id == I::STBU || id == I::LHZU || id == I::LHAU ||
           id == I::STHU || id == I::LDU || id == I::STDU;
}

// Simplified mnemonic stem for a BO/BI pair ("beq", "bdnz", "b"), plus the CR field
// operand ("cr7") when it is not cr0. Empty stem = no simplified form.
struct BranchForm {
    std::string stem;
    std::string field;
};
BranchForm BranchCondition(u32 bo, u32 bi) {
    static constexpr const char* kTrue[] = {"lt", "gt", "eq", "so"};
    static constexpr const char* kFalse[] = {"ge", "le", "ne", "ns"};
    const std::string field = bi / 4 ? Format("cr%u", bi / 4) : std::string();
    switch (bo & 0x1E) {
    case 12: return {std::string("b") + kTrue[bi % 4], field};
    case 4: return {std::string("b") + kFalse[bi % 4], field};
    case 16: return {"bdnz", ""};
    case 18: return {"bdz", ""};
    case 20: return {"b", ""};
    default: return {"", ""};
    }
}

}  // namespace

std::string Disassemble(u32 pc, u32 raw) {
    const Op op{raw};
    const I id = Decode(raw);
    const char* name = PpuInstrName(id);
    const u32 rd = op.rd(), ra = op.ra(), rb = op.rb();

    switch (id) {
    case I::UNK: return Format(".long 0x%08X", raw);
    case I::HLE: return Format("hle #%u  (HLE thunk)", raw & hle::kHleIndexMask);
    case I::SC: return "sc";
    case I::B: {
        const u32 target = static_cast<u32>((op.aa() ? 0 : pc) + op.li());
        return Format("b%s%s 0x%08x", op.lk() ? "l" : "", op.aa() ? "a" : "", target);
    }
    case I::BC: {
        const u32 target = static_cast<u32>((op.aa() ? 0 : pc) + op.bd());
        const BranchForm f = BranchCondition(op.bo(), op.bi());
        const std::string suffix = std::string(op.lk() ? "l" : "") + (op.aa() ? "a" : "");
        if (f.stem.empty()) return Format("bc%s %u,%u,0x%08x", suffix.c_str(), op.bo(), op.bi(), target);
        return Format("%s%s %s%s0x%08x", f.stem.c_str(), suffix.c_str(), f.field.c_str(), f.field.empty() ? "" : ",", target);
    }
    case I::BCLR:
    case I::BCCTR: {
        const char* reg = id == I::BCLR ? "lr" : "ctr";
        const BranchForm f = BranchCondition(op.bo(), op.bi());
        if (f.stem.empty()) return Format("bc%s%s %u,%u", reg, op.lk() ? "l" : "", op.bo(), op.bi());
        return Format("%s%s%s%s%s", f.stem.c_str(), reg, op.lk() ? "l" : "", f.field.empty() ? "" : " ", f.field.c_str());
    }
    case I::MFSPR:
        if (op.spr() == 8) return Format("mflr r%u", rd);
        if (op.spr() == 9) return Format("mfctr r%u", rd);
        if (op.spr() == 1) return Format("mfxer r%u", rd);
        return Format("mfspr r%u,%u", rd, op.spr());
    case I::MTSPR:
        if (op.spr() == 8) return Format("mtlr r%u", rd);
        if (op.spr() == 9) return Format("mtctr r%u", rd);
        if (op.spr() == 1) return Format("mtxer r%u", rd);
        return Format("mtspr %u,r%u", op.spr(), rd);
    case I::MFTB: return Format("mftb r%u", rd);
    case I::MFCR: return Format("mfcr r%u", rd);
    case I::MTCRF: return Format("mtcrf 0x%02x,r%u", op.crm(), rd);
    case I::MCRF: return Format("mcrf cr%u,cr%u", op.crfd(), op.crfs());
    case I::SRAWI: return Format("%s r%u,r%u,%u", Mnemonic(id, op, true, false).c_str(), ra, rd, rb);
    case I::SRADI: return Format("%s r%u,r%u,%u", Mnemonic(id, op, true, false).c_str(), ra, rd, op.sh64());
    case I::TW: case I::TWI: case I::TD: case I::TDI:
        return raw == 0x7FE00008 ? "trap" : Format("%s %u,r%u,...", name, rd, ra);
    default:
        break;
    }

    switch (Classify(id)) {
    case Kind::LoadStoreD:
        return Format("%s r%u,%" PRId64 "(%s)", name, rd, op.simm16(), Ra0(ra).c_str());
    case Kind::LoadStoreDS:
        return Format("%s r%u,%" PRId64 "(%s)", name, rd, op.ds(), Ra0(ra).c_str());
    case Kind::FpLoadStoreD:
        return Format("%s f%u,%" PRId64 "(%s)", name, rd, op.simm16(), Ra0(ra).c_str());
    case Kind::LoadStoreX:
        if (id == I::DCBST || id == I::DCBF || id == I::DCBTST || id == I::DCBT || id == I::DCBZ || id == I::ICBI)
            return Format("%s %s,r%u", name, Ra0(ra).c_str(), rb);
        return Format("%s r%u,%s,r%u", name, rd, Ra0(ra).c_str(), rb);
    case Kind::FpLoadStoreX: return Format("%s f%u,%s,r%u", name, rd, Ra0(ra).c_str(), rb);
    case Kind::VecLoadStoreX: return Format("%s v%u,%s,r%u", name, rd, Ra0(ra).c_str(), rb);
    case Kind::ArithImm:
        if (id == I::ADDI && !ra) return Format("li r%u,%" PRId64, rd, op.simm16());
        if (id == I::ADDIS && !ra) return Format("lis r%u,%" PRId64, rd, op.simm16());
        return Format("%s r%u,r%u,%" PRId64, name, rd, ra, op.simm16());
    case Kind::LogicImm:
        if (raw == 0x60000000) return "nop";
        return Format("%s r%u,r%u,0x%" PRIx64, name, ra, rd, op.uimm16());
    case Kind::CmpImm: {
        const bool logical = id == I::CMPLI;
        const std::string m = std::string("cmp") + (logical ? "l" : "") + (op.l10() ? "d" : "w") + "i";
        if (logical) return Format("%s cr%u,r%u,%" PRIu64, m.c_str(), op.crfd(), ra, op.uimm16());
        return Format("%s cr%u,r%u,%" PRId64, m.c_str(), op.crfd(), ra, op.simm16());
    }
    case Kind::Cmp: {
        const std::string m = std::string("cmp") + (id == I::CMPL ? "l" : "") + (op.l10() ? "d" : "w");
        return Format("%s cr%u,r%u,r%u", m.c_str(), op.crfd(), ra, rb);
    }
    case Kind::XoBinary: {
        const bool has_oe = id != I::MULHW && id != I::MULHWU && id != I::MULHD && id != I::MULHDU;
        return Format("%s r%u,r%u,r%u", Mnemonic(id, op, true, has_oe).c_str(), rd, ra, rb);
    }
    case Kind::XoUnary: return Format("%s r%u,r%u", Mnemonic(id, op, true, true).c_str(), rd, ra);
    case Kind::LogicX:
        if (id == I::OR && rd == rb && !op.rc()) return Format("mr r%u,r%u", ra, rd);
        return Format("%s r%u,r%u,r%u", Mnemonic(id, op, true, false).c_str(), ra, rd, rb);
    case Kind::LogicUnary: return Format("%s r%u,r%u", Mnemonic(id, op, true, false).c_str(), ra, rd);
    case Kind::RotW:
        return Format("%s r%u,r%u,%u,%u,%u", Mnemonic(id, op, true, false).c_str(), ra, rd, rb, op.mb32(), op.me32());
    case Kind::RotWReg:
        return Format("%s r%u,r%u,r%u,%u,%u", Mnemonic(id, op, true, false).c_str(), ra, rd, rb, op.mb32(), op.me32());
    case Kind::RotD:
        return Format("%s r%u,r%u,%u,%u", Mnemonic(id, op, true, false).c_str(), ra, rd, op.sh64(), op.mbe64());
    case Kind::RotDReg:
        return Format("%s r%u,r%u,r%u,%u", Mnemonic(id, op, true, false).c_str(), ra, rd, rb, op.mbe64());
    case Kind::FpBinary: return Format("%s f%u,f%u,f%u", Mnemonic(id, op, true, false).c_str(), rd, ra, rb);
    case Kind::FpMul: return Format("%s f%u,f%u,f%u", Mnemonic(id, op, true, false).c_str(), rd, ra, op.frc());
    case Kind::FpFma:
        return Format("%s f%u,f%u,f%u,f%u", Mnemonic(id, op, true, false).c_str(), rd, ra, op.frc(), rb);
    case Kind::FpUnary: return Format("%s f%u,f%u", Mnemonic(id, op, true, false).c_str(), rd, rb);
    case Kind::FpCmp: return Format("%s cr%u,f%u,f%u", name, op.crfd(), ra, rb);
    case Kind::VecVa: return Format("%s v%u,v%u,v%u,v%u", name, rd, ra, rb, op.frc());
    case Kind::VecVx: return Format("%s v%u,v%u,v%u", name, rd, ra, rb);
    case Kind::Other: break;
    }
    return Format("%s  (0x%08X)", name, raw);
}

std::optional<std::string> ExplainEffectiveAddress(u32 raw, const PpuThreadContext& ctx) {
    const Op op{raw};
    const I id = Decode(raw);
    const Kind kind = Classify(id);
    const u32 ra = op.ra(), rb = op.rb();
    const u64 base = ra ? ctx.gpr[ra] : 0;
    const std::string base_text = ra ? Format("r%u (0x%" PRIx64 ")", ra, base) : std::string("0 (RA field is 0)");
    switch (kind) {
    case Kind::LoadStoreD:
    case Kind::FpLoadStoreD:
        return Format("EA = %s + %" PRId64 " = 0x%08x%s", base_text.c_str(), op.simm16(),
                      static_cast<u32>(base + op.simm16()), IsUpdateOrLoad(id) ? " (update form)" : "");
    case Kind::LoadStoreDS:
        return Format("EA = %s + %" PRId64 " = 0x%08x", base_text.c_str(), op.ds(), static_cast<u32>(base + op.ds()));
    case Kind::LoadStoreX:
    case Kind::FpLoadStoreX:
    case Kind::VecLoadStoreX:
        return Format("EA = %s + r%u (0x%" PRIx64 ") = 0x%08x", base_text.c_str(), rb, ctx.gpr[rb],
                      static_cast<u32>(base + ctx.gpr[rb]));
    default:
        return std::nullopt;
    }
}

}  // namespace cell::ppu
