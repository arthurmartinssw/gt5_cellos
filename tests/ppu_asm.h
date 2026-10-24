// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/ppu_asm.h
//
// Minimal PPC64 encoder for hand-written test programs (checked against real encodings in
// ppu_tests.cpp, TestEncoder).
#pragma once

#include "common/types.h"

using cell::s32;
using cell::u32;

// =======================================================================================
// Encoder
// =======================================================================================
namespace as {
constexpr u32 D(u32 op, u32 rt, u32 ra, s32 imm) { return op << 26 | rt << 21 | ra << 16 | (static_cast<u32>(imm) & 0xFFFF); }
constexpr u32 DS(u32 op, u32 rt, u32 ra, s32 ds, u32 xo) { return op << 26 | rt << 21 | ra << 16 | (static_cast<u32>(ds) & 0xFFFC) | xo; }
constexpr u32 X(u32 rt, u32 ra, u32 rb, u32 xo, u32 rc = 0) { return 31u << 26 | rt << 21 | ra << 16 | rb << 11 | xo << 1 | rc; }
constexpr u32 XO(u32 rt, u32 ra, u32 rb, u32 xo, u32 oe = 0, u32 rc = 0) { return 31u << 26 | rt << 21 | ra << 16 | rb << 11 | oe << 10 | xo << 1 | rc; }
constexpr u32 M(u32 op, u32 rs, u32 ra, u32 sh, u32 mb, u32 me, u32 rc = 0) { return op << 26 | rs << 21 | ra << 16 | sh << 11 | mb << 6 | me << 1 | rc; }
constexpr u32 MD(u32 rs, u32 ra, u32 sh, u32 mb, u32 xo, u32 rc = 0) {
    return 30u << 26 | rs << 21 | ra << 16 | (sh & 31) << 11 | (mb & 31) << 6 | (mb & 32) | xo << 2 | (sh >> 5) << 1 | rc;
}
constexpr u32 SPR(u32 rt, u32 spr, u32 xo) { return 31u << 26 | rt << 21 | (((spr & 31) << 5) | (spr >> 5)) << 11 | xo << 1; }
constexpr u32 A(u32 op, u32 frt, u32 fra, u32 frb, u32 frc, u32 xo) { return op << 26 | frt << 21 | fra << 16 | frb << 11 | frc << 6 | xo << 1; }
constexpr u32 VX(u32 vt, u32 va, u32 vb, u32 xo) { return 4u << 26 | vt << 21 | va << 16 | vb << 11 | xo; }
constexpr u32 VA(u32 vt, u32 va, u32 vb, u32 vc, u32 xo) { return 4u << 26 | vt << 21 | va << 16 | vb << 11 | vc << 6 | xo; }

constexpr u32 li(u32 rt, s32 v) { return D(14, rt, 0, v); }
constexpr u32 lis(u32 rt, s32 v) { return D(15, rt, 0, v); }
constexpr u32 addi(u32 rt, u32 ra, s32 v) { return D(14, rt, ra, v); }
constexpr u32 ori(u32 ra, u32 rs, u32 v) { return D(24, rs, ra, static_cast<s32>(v)); }
constexpr u32 oris(u32 ra, u32 rs, u32 v) { return D(25, rs, ra, static_cast<s32>(v)); }
constexpr u32 cmpwi(u32 cr, u32 ra, s32 v) { return D(11, cr << 2, ra, v); }
constexpr u32 cmpdi(u32 cr, u32 ra, s32 v) { return D(11, cr << 2 | 1, ra, v); }
constexpr u32 cmplwi(u32 cr, u32 ra, u32 v) { return D(10, cr << 2, ra, static_cast<s32>(v)); }
constexpr u32 lwz(u32 rt, s32 d, u32 ra) { return D(32, rt, ra, d); }
constexpr u32 lwzu(u32 rt, s32 d, u32 ra) { return D(33, rt, ra, d); }
constexpr u32 lbz(u32 rt, s32 d, u32 ra) { return D(34, rt, ra, d); }
constexpr u32 stw(u32 rs, s32 d, u32 ra) { return D(36, rs, ra, d); }
constexpr u32 stwu(u32 rs, s32 d, u32 ra) { return D(37, rs, ra, d); }
constexpr u32 stb(u32 rs, s32 d, u32 ra) { return D(38, rs, ra, d); }
constexpr u32 lhz(u32 rt, s32 d, u32 ra) { return D(40, rt, ra, d); }
constexpr u32 lha(u32 rt, s32 d, u32 ra) { return D(42, rt, ra, d); }
constexpr u32 sth(u32 rs, s32 d, u32 ra) { return D(44, rs, ra, d); }
constexpr u32 lmw(u32 rt, s32 d, u32 ra) { return D(46, rt, ra, d); }
constexpr u32 stmw(u32 rs, s32 d, u32 ra) { return D(47, rs, ra, d); }
constexpr u32 lfs(u32 frt, s32 d, u32 ra) { return D(48, frt, ra, d); }
constexpr u32 lfd(u32 frt, s32 d, u32 ra) { return D(50, frt, ra, d); }
constexpr u32 stfs(u32 frs, s32 d, u32 ra) { return D(52, frs, ra, d); }
constexpr u32 stfd(u32 frs, s32 d, u32 ra) { return D(54, frs, ra, d); }
constexpr u32 ld(u32 rt, s32 ds, u32 ra) { return DS(58, rt, ra, ds, 0); }
constexpr u32 lwa(u32 rt, s32 ds, u32 ra) { return DS(58, rt, ra, ds, 2); }
constexpr u32 std_(u32 rs, s32 ds, u32 ra) { return DS(62, rs, ra, ds, 0); }
constexpr u32 stdu(u32 rs, s32 ds, u32 ra) { return DS(62, rs, ra, ds, 1); }
constexpr u32 mr(u32 ra, u32 rs) { return X(rs, ra, rs, 444); }
constexpr u32 mflr(u32 rt) { return SPR(rt, 8, 339); }
constexpr u32 mtlr(u32 rs) { return SPR(rs, 8, 467); }
constexpr u32 mtctr(u32 rs) { return SPR(rs, 9, 467); }
constexpr u32 mfctr(u32 rt) { return SPR(rt, 9, 339); }
constexpr u32 mfxer(u32 rt) { return SPR(rt, 1, 339); }
constexpr u32 b(s32 off) { return 18u << 26 | (static_cast<u32>(off) & 0x03FFFFFC); }
constexpr u32 bl(s32 off) { return b(off) | 1; }
constexpr u32 bc(u32 bo, u32 bi, s32 off) { return 16u << 26 | bo << 21 | bi << 16 | (static_cast<u32>(off) & 0xFFFC); }
constexpr u32 bne(u32 cr, s32 off) { return bc(4, cr * 4 + 2, off); }
constexpr u32 beq(u32 cr, s32 off) { return bc(12, cr * 4 + 2, off); }
constexpr u32 bdnz(s32 off) { return bc(16, 0, off); }
constexpr u32 blr() { return 0x4E800020; }
constexpr u32 bctrl() { return 0x4E800421; }
constexpr u32 sc() { return 0x44000002; }
constexpr u32 nop() { return 0x60000000; }
constexpr u32 rlwinm(u32 ra, u32 rs, u32 sh, u32 mb, u32 me) { return M(21, rs, ra, sh, mb, me); }
constexpr u32 rlwimi(u32 ra, u32 rs, u32 sh, u32 mb, u32 me) { return M(20, rs, ra, sh, mb, me); }
constexpr u32 rldicl(u32 ra, u32 rs, u32 sh, u32 mb) { return MD(rs, ra, sh, mb, 0); }
constexpr u32 rldicr(u32 ra, u32 rs, u32 sh, u32 me) { return MD(rs, ra, sh, me, 1); }
constexpr u32 rldic(u32 ra, u32 rs, u32 sh, u32 mb) { return MD(rs, ra, sh, mb, 2); }
constexpr u32 rldimi(u32 ra, u32 rs, u32 sh, u32 mb) { return MD(rs, ra, sh, mb, 3); }
constexpr u32 sradi(u32 ra, u32 rs, u32 sh) { return 31u << 26 | rs << 21 | ra << 16 | (sh & 31) << 11 | 413u << 2 | (sh >> 5) << 1; }
constexpr u32 srawi(u32 ra, u32 rs, u32 sh) { return X(rs, ra, sh, 824); }
constexpr u32 extsw(u32 ra, u32 rs) { return X(rs, ra, 0, 986); }
}  // namespace as


// Program builder with labels for branchy test code.
#include <map>
#include <string>
#include <vector>

class Program {
public:
    Program& operator<<(u32 word) {
        code_.push_back(word);
        return *this;
    }
    Program& label(const std::string& name) {
        labels_[name] = static_cast<u32>(code_.size());
        return *this;
    }
    // Conditional/unconditional branches to a label, resolved in words().
    Program& b(const std::string& target) { return branch(target, 18u << 26, false); }
    Program& bne(u32 cr, const std::string& target) { return branch(target, as::bc(4, cr * 4 + 2, 0), true); }
    Program& beq(u32 cr, const std::string& target) { return branch(target, as::bc(12, cr * 4 + 2, 0), true); }
    // Calls an HLE import whose OPD pointer sits at [rBase + slot]: lwz r12; lwz r0; mtctr; bctrl.
    Program& call_import(u32 base_reg, s32 slot) {
        return *this << as::lwz(12, slot, base_reg) << as::lwz(0, 0, 12) << as::mtctr(0) << as::bctrl();
    }
    Program& syscall(u32 number) { return *this << as::li(11, static_cast<s32>(number)) << as::sc(); }

    std::vector<u32> words() const {
        std::vector<u32> out = code_;
        for (const auto& [at, target, conditional] : fixups_) {
            const s32 off = (static_cast<s32>(labels_.at(target)) - static_cast<s32>(at)) * 4;
            out[at] |= conditional ? (static_cast<u32>(off) & 0xFFFC) : (static_cast<u32>(off) & 0x03FFFFFC);
        }
        return out;
    }

private:
    Program& branch(const std::string& target, u32 word, bool conditional) {
        fixups_.push_back({static_cast<u32>(code_.size()), target, conditional});
        code_.push_back(word);
        return *this;
    }
    struct Fixup {
        u32 at;
        std::string target;
        bool conditional;
    };
    std::vector<u32> code_;
    std::map<std::string, u32> labels_;
    std::vector<Fixup> fixups_;
};
