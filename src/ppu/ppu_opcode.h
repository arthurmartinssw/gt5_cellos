// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — ppu/ppu_opcode.h
//
// Field extraction for 32-bit PowerPC instruction words (Cell PPU = PowerPC 2.02 + VMX).
//
// IBM numbers bits from the left: bit 0 is the MSB of the word. A field occupying IBM bits
// [a, b] is therefore (raw >> (31 - b)) & ((1 << (b - a + 1)) - 1). Every accessor below is
// written in that form, with the IBM bit range in the comment, so it can be checked against
// the "PowerPC User Instruction Set Architecture" book field by field.
//
// The instruction word arrives here already byte-swapped (host order): memory holds it
// big-endian, the predecoder swaps once per instruction when it fills a page.
#pragma once

#include "common/types.h"

namespace cell::ppu {

struct Op {
    u32 raw;

    // ---- common register fields ----
    [[nodiscard]] constexpr u32 opcd() const noexcept { return raw >> 26; }          // 0-5
    [[nodiscard]] constexpr u32 rd() const noexcept { return (raw >> 21) & 31; }     // 6-10 (RT/RS/FRT/VRT/BO/TO)
    [[nodiscard]] constexpr u32 ra() const noexcept { return (raw >> 16) & 31; }     // 11-15 (RA/BI/VRA)
    [[nodiscard]] constexpr u32 rb() const noexcept { return (raw >> 11) & 31; }     // 16-20 (RB/SH/VRB)
    [[nodiscard]] constexpr u32 frc() const noexcept { return (raw >> 6) & 31; }     // 21-25 (FRC/VRC/MB)
    [[nodiscard]] constexpr u32 me32() const noexcept { return (raw >> 1) & 31; }    // 26-30 (ME)
    [[nodiscard]] constexpr bool rc() const noexcept { return raw & 1; }             // 31 (Rc / LK)
    [[nodiscard]] constexpr bool oe() const noexcept { return (raw >> 10) & 1; }     // 21 (OE)

    // ---- immediates ----
    [[nodiscard]] constexpr s64 simm16() const noexcept { return static_cast<s16>(raw & 0xFFFF); }   // 16-31
    [[nodiscard]] constexpr u64 uimm16() const noexcept { return raw & 0xFFFF; }
    [[nodiscard]] constexpr s64 ds() const noexcept { return static_cast<s16>(raw & 0xFFFC); }       // 16-29, <<2

    // ---- branches ----
    [[nodiscard]] constexpr u32 bo() const noexcept { return rd(); }
    [[nodiscard]] constexpr u32 bi() const noexcept { return ra(); }
    [[nodiscard]] constexpr s32 bd() const noexcept { return static_cast<s16>(raw & 0xFFFC); }       // 16-29
    [[nodiscard]] constexpr s32 li() const noexcept {                                                 // 6-29
        return static_cast<s32>((raw & 0x03FFFFFCu) << 6) >> 6;
    }
    [[nodiscard]] constexpr bool aa() const noexcept { return (raw >> 1) & 1; }     // 30
    [[nodiscard]] constexpr bool lk() const noexcept { return raw & 1; }            // 31

    // ---- condition register ----
    [[nodiscard]] constexpr u32 crfd() const noexcept { return (raw >> 23) & 7; }   // 6-8
    [[nodiscard]] constexpr u32 crfs() const noexcept { return (raw >> 18) & 7; }   // 11-13
    [[nodiscard]] constexpr bool l10() const noexcept { return (raw >> 21) & 1; }   // 10 (cmp L: 1 = doubleword)
    [[nodiscard]] constexpr u32 crm() const noexcept { return (raw >> 12) & 0xFF; } // 12-19

    // ---- rotates ----
    [[nodiscard]] constexpr u32 sh32() const noexcept { return rb(); }
    [[nodiscard]] constexpr u32 mb32() const noexcept { return frc(); }
    // MD/MDS/XS-forms split the 6-bit value: sh = sh[0:4] (bits 16-20) || sh[5] (bit 30),
    // mb/me = field bits 21-25 low part || bit 26 high part.
    [[nodiscard]] constexpr u32 sh64() const noexcept { return ((raw >> 11) & 31) | ((raw & 2) << 4); }
    [[nodiscard]] constexpr u32 mbe64() const noexcept { return ((raw >> 6) & 31) | (raw & 0x20); }

    // ---- special purpose registers: the two 5-bit halves are swapped in the encoding ----
    [[nodiscard]] constexpr u32 spr() const noexcept { return ((raw >> 16) & 31) | (((raw >> 11) & 31) << 5); }

    // ---- FPU ----
    [[nodiscard]] constexpr u32 flm() const noexcept { return (raw >> 17) & 0xFF; } // 7-14
    [[nodiscard]] constexpr u32 fpimm() const noexcept { return (raw >> 12) & 0xF; }// 16-19

    // ---- VMX ----
    [[nodiscard]] constexpr u32 vuimm() const noexcept { return ra(); }
    [[nodiscard]] constexpr s32 vsimm() const noexcept { return static_cast<s32>(ra() << 27) >> 27; }
    [[nodiscard]] constexpr u32 vshb() const noexcept { return (raw >> 6) & 15; }   // 22-25
    [[nodiscard]] constexpr bool vrc() const noexcept { return (raw >> 10) & 1; }   // 21
};

// Secondary opcode extraction per form, used by the decoder.
[[nodiscard]] constexpr u32 XoX(u32 raw) noexcept { return (raw >> 1) & 0x3FF; }  // X/XL/XFX: bits 21-30
[[nodiscard]] constexpr u32 XoA(u32 raw) noexcept { return (raw >> 1) & 0x1F; }   // A-form: bits 26-30
[[nodiscard]] constexpr u32 XoVA(u32 raw) noexcept { return raw & 0x3F; }         // VA-form: bits 26-31
[[nodiscard]] constexpr u32 XoVX(u32 raw) noexcept { return raw & 0x7FF; }        // VX/VC-form: bits 21-31

// Masks in IBM numbering for 64-bit registers: bits mb..me set, wrapping when mb > me.
[[nodiscard]] constexpr u64 Mask64(u32 mb, u32 me) noexcept {
    const u64 a = ~u64{0} >> mb;
    const u64 b = ~u64{0} << (63 - me);
    return mb <= me ? (a & b) : (a | b);
}

[[nodiscard]] constexpr u64 Rotl64(u64 v, u32 n) noexcept {
    n &= 63;
    return n ? (v << n) | (v >> (64 - n)) : v;
}
// 32-bit rotate with the result replicated in both halves, as the rlw* instructions define
// it in 64-bit mode (ROTL32(x, n) = ROTL64(x || x, n)).
[[nodiscard]] constexpr u64 Rotl32Dup(u64 v, u32 n) noexcept {
    const u64 x = v & 0xFFFFFFFFu;
    return Rotl64(x | (x << 32), n);
}

}  // namespace cell::ppu
