// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — ppu/ppu_decoder.cpp
#include "ppu/ppu_decoder.h"

#include <array>
#include <cstdio>

#include "ppu/ppu_opcode.h"

namespace cell::ppu {

namespace {

using I = PpuInstr;

struct Tables {
    std::array<I, 64> primary{};
    std::array<I, 64> op4_va{};
    std::array<I, 2048> op4_vx{};
    std::array<I, 1024> op19{};
    std::array<I, 16> op30{};   // indexed by bits 27-30 (MDS xo); MD xo occupies the top 3
    std::array<I, 1024> op31{};
    std::array<I, 4> op58{};
    std::array<I, 4> op62{};
    std::array<I, 32> op59{};
    std::array<I, 32> op63_a{};
    std::array<I, 1024> op63_x{};

    Tables() {
        primary.fill(I::UNK);
        op4_va.fill(I::UNK);
        op4_vx.fill(I::UNK);
        op19.fill(I::UNK);
        op30.fill(I::UNK);
        op31.fill(I::UNK);
        op58.fill(I::UNK);
        op62.fill(I::UNK);
        op59.fill(I::UNK);
        op63_a.fill(I::UNK);
        op63_x.fill(I::UNK);

        // ---------------- primary ----------------
        primary[1] = I::HLE;  // our thunk opcode (unused by the PPU ISA)
        primary[2] = I::TDI;
        primary[3] = I::TWI;
        primary[7] = I::MULLI;
        primary[8] = I::SUBFIC;
        primary[10] = I::CMPLI;
        primary[11] = I::CMPI;
        primary[12] = I::ADDIC;
        primary[13] = I::ADDIC_;
        primary[14] = I::ADDI;
        primary[15] = I::ADDIS;
        primary[16] = I::BC;
        primary[17] = I::SC;
        primary[18] = I::B;
        primary[20] = I::RLWIMI;
        primary[21] = I::RLWINM;
        primary[23] = I::RLWNM;
        primary[24] = I::ORI;
        primary[25] = I::ORIS;
        primary[26] = I::XORI;
        primary[27] = I::XORIS;
        primary[28] = I::ANDI_;
        primary[29] = I::ANDIS_;
        primary[32] = I::LWZ;
        primary[33] = I::LWZU;
        primary[34] = I::LBZ;
        primary[35] = I::LBZU;
        primary[36] = I::STW;
        primary[37] = I::STWU;
        primary[38] = I::STB;
        primary[39] = I::STBU;
        primary[40] = I::LHZ;
        primary[41] = I::LHZU;
        primary[42] = I::LHA;
        primary[43] = I::LHAU;
        primary[44] = I::STH;
        primary[45] = I::STHU;
        primary[46] = I::LMW;
        primary[47] = I::STMW;
        primary[48] = I::LFS;
        primary[49] = I::LFSU;
        primary[50] = I::LFD;
        primary[51] = I::LFDU;
        primary[52] = I::STFS;
        primary[53] = I::STFSU;
        primary[54] = I::STFD;
        primary[55] = I::STFDU;

        // ---------------- 19: XL-form ----------------
        op19[0] = I::MCRF;
        op19[16] = I::BCLR;
        op19[33] = I::CRNOR;
        op19[129] = I::CRANDC;
        op19[150] = I::ISYNC;
        op19[193] = I::CRXOR;
        op19[225] = I::CRNAND;
        op19[257] = I::CRAND;
        op19[289] = I::CREQV;
        op19[417] = I::CRORC;
        op19[449] = I::CROR;
        op19[528] = I::BCCTR;

        // ---------------- 30: MD (xo<<1 | sh5 don't care) / MDS ----------------
        for (u32 sh5 = 0; sh5 < 2; ++sh5) {
            op30[(0 << 1) | sh5] = I::RLDICL;
            op30[(1 << 1) | sh5] = I::RLDICR;
            op30[(2 << 1) | sh5] = I::RLDIC;
            op30[(3 << 1) | sh5] = I::RLDIMI;
        }
        op30[8] = I::RLDCL;
        op30[9] = I::RLDCR;

        // ---------------- 31: X / XO / XS / XFX ----------------
        auto xo = [&](u32 x, I id) { op31[x] = id; };
        auto xo_oe = [&](u32 x, I id) {  // XO-form: OE (bit 21) is part of the 10-bit field
            op31[x] = id;
            op31[x | 0x200] = id;
        };
        xo(0, I::CMP);
        xo(4, I::TW);
        xo(6, I::LVSL);
        xo(7, I::LVEBX);
        xo_oe(8, I::SUBFC);
        xo(9, I::MULHDU);
        xo_oe(10, I::ADDC);
        xo(11, I::MULHWU);
        xo(19, I::MFCR);  // also mfocrf (bit 11 set)
        xo(20, I::LWARX);
        xo(21, I::LDX);
        xo(23, I::LWZX);
        xo(24, I::SLW);
        xo(26, I::CNTLZW);
        xo(27, I::SLD);
        xo(28, I::AND);
        xo(32, I::CMPL);
        xo(38, I::LVSR);
        xo(39, I::LVEHX);
        xo_oe(40, I::SUBF);
        xo(53, I::LDUX);
        xo(54, I::DCBST);
        xo(55, I::LWZUX);
        xo(58, I::CNTLZD);
        xo(60, I::ANDC);
        xo(68, I::TD);
        xo(71, I::LVEWX);
        xo(73, I::MULHD);
        xo(75, I::MULHW);
        xo(84, I::LDARX);
        xo(86, I::DCBF);
        xo(87, I::LBZX);
        xo(103, I::LVX);
        xo_oe(104, I::NEG);
        xo(119, I::LBZUX);
        xo(124, I::NOR);
        xo(135, I::STVEBX);
        xo_oe(136, I::SUBFE);
        xo_oe(138, I::ADDE);
        xo(144, I::MTCRF);  // also mtocrf
        xo(149, I::STDX);
        xo(150, I::STWCX);
        xo(151, I::STWX);
        xo(167, I::STVEHX);
        xo(181, I::STDUX);
        xo(183, I::STWUX);
        xo(199, I::STVEWX);
        xo_oe(200, I::SUBFZE);
        xo_oe(202, I::ADDZE);
        xo(214, I::STDCX);
        xo(215, I::STBX);
        xo(231, I::STVX);
        xo_oe(232, I::SUBFME);
        xo_oe(233, I::MULLD);
        xo_oe(234, I::ADDME);
        xo_oe(235, I::MULLW);
        xo(246, I::DCBTST);
        xo(247, I::STBUX);
        xo_oe(266, I::ADD);
        xo(278, I::DCBT);
        xo(279, I::LHZX);
        xo(284, I::EQV);
        xo(311, I::LHZUX);
        xo(316, I::XOR);
        xo(339, I::MFSPR);
        xo(341, I::LWAX);
        xo(342, I::DST);
        xo(343, I::LHAX);
        xo(359, I::LVX);  // lvxl: same data, LRU hint only
        xo(371, I::MFTB);
        xo(373, I::LWAUX);
        xo(374, I::DSTST);
        xo(375, I::LHAUX);
        xo(407, I::STHX);
        xo(412, I::ORC);
        xo(439, I::STHUX);
        xo(444, I::OR);
        xo_oe(457, I::DIVDU);
        xo_oe(459, I::DIVWU);
        xo(467, I::MTSPR);
        xo(476, I::NAND);
        xo(487, I::STVX);  // stvxl
        xo_oe(489, I::DIVD);
        xo_oe(491, I::DIVW);
        xo(519, I::LVLX);
        xo(534, I::LWBRX);
        xo(535, I::LFSX);
        xo(536, I::SRW);
        xo(539, I::SRD);
        xo(551, I::LVRX);
        xo(567, I::LFSUX);
        xo(598, I::SYNC);  // sync / lwsync
        xo(599, I::LFDX);
        xo(631, I::LFDUX);
        xo(647, I::STVLX);
        xo(662, I::STWBRX);
        xo(663, I::STFSX);
        xo(679, I::STVRX);
        xo(695, I::STFSUX);
        xo(727, I::STFDX);
        xo(759, I::STFDUX);
        xo(775, I::LVLX);   // lvlxl
        xo(790, I::LHBRX);
        xo(792, I::SRAW);
        xo(794, I::SRAD);
        xo(807, I::LVRX);   // lvrxl
        xo(822, I::DSS);
        xo(824, I::SRAWI);
        xo(826, I::SRADI);  // XS-form: 9-bit xo 413, bit 30 = sh[5]
        xo(827, I::SRADI);
        xo(854, I::EIEIO);
        xo(903, I::STVLX);  // stvlxl
        xo(918, I::STHBRX);
        xo(922, I::EXTSH);
        xo(935, I::STVRX);  // stvrxl
        xo(954, I::EXTSB);
        xo(982, I::ICBI);
        xo(983, I::STFIWX);
        xo(986, I::EXTSW);
        xo(1014, I::DCBZ);

        // ---------------- 58 / 62: DS-form ----------------
        op58[0] = I::LD;
        op58[1] = I::LDU;
        op58[2] = I::LWA;
        op62[0] = I::STD;
        op62[1] = I::STDU;

        // ---------------- 59: single-precision A-form ----------------
        op59[18] = I::FDIVS;
        op59[20] = I::FSUBS;
        op59[21] = I::FADDS;
        op59[22] = I::FSQRTS;
        op59[24] = I::FRES;
        op59[25] = I::FMULS;
        op59[28] = I::FMSUBS;
        op59[29] = I::FMADDS;
        op59[30] = I::FNMSUBS;
        op59[31] = I::FNMADDS;

        // ---------------- 63: double A-form, then X-form ----------------
        op63_a[18] = I::FDIV;
        op63_a[20] = I::FSUB;
        op63_a[21] = I::FADD;
        op63_a[22] = I::FSQRT;
        op63_a[23] = I::FSEL;
        op63_a[25] = I::FMUL;
        op63_a[26] = I::FRSQRTE;
        op63_a[28] = I::FMSUB;
        op63_a[29] = I::FMADD;
        op63_a[30] = I::FNMSUB;
        op63_a[31] = I::FNMADD;
        op63_x[0] = I::FCMPU;
        op63_x[12] = I::FRSP;
        op63_x[14] = I::FCTIW;
        op63_x[15] = I::FCTIWZ;
        op63_x[32] = I::FCMPO;
        op63_x[38] = I::MTFSB1;
        op63_x[40] = I::FNEG;
        op63_x[64] = I::MCRFS;
        op63_x[70] = I::MTFSB0;
        op63_x[72] = I::FMR;
        op63_x[134] = I::MTFSFI;
        op63_x[136] = I::FNABS;
        op63_x[264] = I::FABS;
        op63_x[583] = I::MFFS;
        op63_x[711] = I::MTFSF;
        op63_x[814] = I::FCTID;
        op63_x[815] = I::FCTIDZ;
        op63_x[846] = I::FCFID;

        // ---------------- 4: VMX ----------------
        op4_va[42] = I::VSEL;
        op4_va[43] = I::VPERM;
        op4_va[44] = I::VSLDOI;
        op4_va[46] = I::VMADDFP;
        op4_va[47] = I::VNMSUBFP;
        auto vx = [&](u32 x, I id) { op4_vx[x] = id; };
        auto vc = [&](u32 x, I id) {  // VC-form: Rc (bit 21) inside the 11-bit field
            op4_vx[x] = id;
            op4_vx[x | 0x400] = id;
        };
        vx(0, I::VADDUBM);
        vx(64, I::VADDUHM);
        vx(128, I::VADDUWM);
        vx(10, I::VADDFP);
        vx(1024, I::VSUBUBM);
        vx(1088, I::VSUBUHM);
        vx(1152, I::VSUBUWM);
        vx(74, I::VSUBFP);
        vx(1028, I::VAND);
        vx(1092, I::VANDC);
        vx(1156, I::VOR);
        vx(1284, I::VNOR);
        vx(1220, I::VXOR);
        vx(524, I::VSPLTB);
        vx(588, I::VSPLTH);
        vx(652, I::VSPLTW);
        vx(780, I::VSPLTISB);
        vx(844, I::VSPLTISH);
        vx(908, I::VSPLTISW);
        vx(12, I::VMRGHB);
        vx(76, I::VMRGHH);
        vx(140, I::VMRGHW);
        vx(268, I::VMRGLB);
        vx(332, I::VMRGLH);
        vx(396, I::VMRGLW);
        vx(388, I::VSLW);
        vx(644, I::VSRW);
        vx(900, I::VSRAW);
        vx(132, I::VRLW);
        vx(1034, I::VMAXFP);
        vx(1098, I::VMINFP);
        vx(386, I::VMAXSW);
        vx(898, I::VMINSW);
        vx(130, I::VMAXUW);
        vx(642, I::VMINUW);
        vx(842, I::VCFSX);
        vx(778, I::VCFUX);
        vx(970, I::VCTSXS);
        vx(906, I::VCTUXS);
        vx(266, I::VREFP);
        vx(330, I::VRSQRTEFP);
        vx(1540, I::MFVSCR);
        vx(1604, I::MTVSCR);
        vc(6, I::VCMPEQUB);
        vc(70, I::VCMPEQUH);
        vc(134, I::VCMPEQUW);
        vc(198, I::VCMPEQFP);
        vc(454, I::VCMPGEFP);
        vc(710, I::VCMPGTFP);
        vc(518, I::VCMPGTUB);
        vc(582, I::VCMPGTUH);
        vc(646, I::VCMPGTUW);
        vc(774, I::VCMPGTSB);
        vc(838, I::VCMPGTSH);
        vc(902, I::VCMPGTSW);
    }
};

const Tables& GetTables() {
    static const Tables tables;  // built once, read-only afterwards (thread-safe init)
    return tables;
}

}  // namespace

PpuInstr Decode(u32 raw) noexcept {
    const Tables& t = GetTables();
    switch (raw >> 26) {
    case 4:
        return (raw & 0x20) ? t.op4_va[XoVA(raw)] : t.op4_vx[XoVX(raw)];
    case 19:
        return t.op19[XoX(raw)];
    case 30:
        return t.op30[(raw >> 1) & 0xF];
    case 31:
        return t.op31[XoX(raw)];
    case 58:
        return t.op58[raw & 3];
    case 62:
        return t.op62[raw & 3];
    case 59:
        return t.op59[XoA(raw)];
    case 63: {
        const PpuInstr a = t.op63_a[XoA(raw)];
        return a != PpuInstr::UNK ? a : t.op63_x[XoX(raw)];
    }
    default:
        return t.primary[raw >> 26];
    }
}

std::string DescribeEncoding(u32 raw) {
    char text[96];
    const u32 primary = raw >> 26;
    switch (primary) {
    case 4:
        std::snprintf(text, sizeof(text), "primary 4 (VMX), %s xo %u", (raw & 0x20) ? "VA" : "VX", (raw & 0x20) ? XoVA(raw) : XoVX(raw));
        break;
    case 19:
    case 31:
        std::snprintf(text, sizeof(text), "primary %u, xo %u", primary, XoX(raw));
        break;
    case 30:
        std::snprintf(text, sizeof(text), "primary 30, xo %u", (raw >> 1) & 0xF);
        break;
    case 58:
    case 62:
        std::snprintf(text, sizeof(text), "primary %u, xo %u", primary, raw & 3);
        break;
    case 59:
    case 63:
        std::snprintf(text, sizeof(text), "primary %u, A-xo %u / X-xo %u", primary, XoA(raw), XoX(raw));
        break;
    default:
        std::snprintf(text, sizeof(text), "primary %u", primary);
    }
    return text;
}

}  // namespace cell::ppu
