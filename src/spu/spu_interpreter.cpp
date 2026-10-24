// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — spu/spu_interpreter.cpp
#include "spu/spu_interpreter.h"

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "memory/guest_copy.h"
#include "spu/spu_decoder.h"

namespace cell::spu {

namespace {

using V = ppu::v128;

// ---- register views (architectural big-endian order) ----
struct W4 {
    u32 w[4];
};
inline W4 Words(const V& v) {
    W4 r;
    std::memcpy(r.w, v.bytes, 16);
    for (u32& x : r.w) x = ByteSwap(x);
    return r;
}
inline void SetWords(V& v, W4 r) {
    for (u32& x : r.w) x = ByteSwap(x);
    std::memcpy(v.bytes, r.w, 16);
}
inline u32 Pref(const V& v) { return Words(v).w[0]; }
inline void SetPref(V& v, u32 x) { SetWords(v, {{x, 0, 0, 0}}); }
inline u16 Half(const V& v, int i) { return static_cast<u16>(v.bytes[2 * i] << 8 | v.bytes[2 * i + 1]); }
inline void SetHalf(V& v, int i, u16 x) {
    v.bytes[2 * i] = static_cast<u8>(x >> 8);
    v.bytes[2 * i + 1] = static_cast<u8>(x);
}
inline u64 Dword(const W4& q, int i) { return u64{q.w[2 * i]} << 32 | q.w[2 * i + 1]; }
inline void SetDword(W4& q, int i, u64 x) {
    q.w[2 * i] = static_cast<u32>(x >> 32);
    q.w[2 * i + 1] = static_cast<u32>(x);
}

template <typename F>
inline void MapWords(V& t, const V& a, const V& b, F f) {
    const W4 qa = Words(a), qb = Words(b);
    W4 r;
    for (int i = 0; i < 4; ++i) r.w[i] = f(qa.w[i], qb.w[i]);
    SetWords(t, r);
}
template <typename F>
inline void MapWords1(V& t, const V& a, F f) {
    const W4 qa = Words(a);
    W4 r;
    for (int i = 0; i < 4; ++i) r.w[i] = f(qa.w[i]);
    SetWords(t, r);
}
template <typename F>
inline void MapHalves(V& t, const V& a, const V& b, F f) {
    V r;
    for (int i = 0; i < 8; ++i) SetHalf(r, i, static_cast<u16>(f(Half(a, i), Half(b, i))));
    t = r;
}
template <typename F>
inline void MapBytes(V& t, const V& a, const V& b, F f) {
    V r;
    for (int i = 0; i < 16; ++i) r.bytes[i] = static_cast<u8>(f(a.bytes[i], b.bytes[i]));
    t = r;
}

// ---- quadword shifts on the big-endian byte array ----
inline V ShiftLeftBytes(const V& a, u32 n) {
    V r{};
    for (u32 i = 0; i + n < 16; ++i) r.bytes[i] = a.bytes[i + n];
    return r;
}
inline V ShiftRightBytes(const V& a, u32 n) {
    V r{};
    for (u32 i = n; i < 16; ++i) r.bytes[i] = a.bytes[i - n];
    return r;
}
inline V RotateLeftBytes(const V& a, u32 n) {
    V r;
    for (u32 i = 0; i < 16; ++i) r.bytes[i] = a.bytes[(i + n) & 15];
    return r;
}
inline V ShiftLeftBits(const V& a, u32 s) {  // s in 0..7
    if (!s) return a;
    V r;
    for (u32 i = 0; i < 16; ++i) r.bytes[i] = static_cast<u8>(a.bytes[i] << s | (i < 15 ? a.bytes[i + 1] >> (8 - s) : 0));
    return r;
}
inline V RotateLeftBits(const V& a, u32 s) {
    if (!s) return a;
    V r;
    for (u32 i = 0; i < 16; ++i) r.bytes[i] = static_cast<u8>(a.bytes[i] << s | a.bytes[(i + 1) & 15] >> (8 - s));
    return r;
}
inline V ShiftRightBits(const V& a, u32 s) {
    if (!s) return a;
    V r;
    for (u32 i = 0; i < 16; ++i) r.bytes[i] = static_cast<u8>(a.bytes[i] >> s | (i > 0 ? a.bytes[i - 1] << (8 - s) : 0));
    return r;
}

// ---- single precision with SPU conventions (denormals flushed, no infinities) ----
inline float F(u32 x) {
    if ((x & 0x7F800000u) == 0) x &= 0x80000000u;  // denormal -> signed zero
    return std::bit_cast<float>(x);
}
inline u32 Fbits(float f) {
    u32 x = std::bit_cast<u32>(f);
    if ((x & 0x7F800000u) == 0) return x & 0x80000000u;
    if ((x & 0x7F800000u) == 0x7F800000u) return (x & 0x80000000u) | 0x7F7FFFFFu;  // saturate (also NaN)
    return x;
}
inline double D(u64 x) { return std::bit_cast<double>(x); }
inline u64 Dbits(double d) { return std::bit_cast<u64>(d); }

u32 ToSigned(double v) {
    if (std::isnan(v)) return 0;
    if (v >= 2147483647.0) return 0x7FFFFFFFu;
    if (v <= -2147483648.0) return 0x80000000u;
    return static_cast<u32>(static_cast<s32>(v));
}
u32 ToUnsigned(double v) {
    if (std::isnan(v) || v <= 0.0) return 0;
    if (v >= 4294967295.0) return 0xFFFFFFFFu;
    return static_cast<u32>(v);
}

inline u32 Mask(bool c) { return c ? 0xFFFFFFFFu : 0; }

// Insertion-control default pattern (cbd/chd/cwd/cdd and the x forms).
V InsertionControl(u32 offset, u32 size) {
    V r;
    for (u32 i = 0; i < 16; ++i) r.bytes[i] = static_cast<u8>(0x10 + i);
    const u32 at = offset & (16 - size) & 0xF;
    for (u32 i = 0; i < size; ++i) r.bytes[at + i] = static_cast<u8>((size == 1 ? 3 : size == 2 ? 2 : 0) + i);
    return r;
}

u32 FetchWord(const u8* ls, u32 pc) {
    return ByteSwap(std::atomic_ref<u32>(*reinterpret_cast<u32*>(const_cast<u8*>(ls) + pc)).load(std::memory_order_relaxed));
}

}  // namespace

std::unique_ptr<ISpuExecutor> MakeSpuInterpreter(SpuThread&) { return std::make_unique<SpuInterpreter>(); }

void SpuInterpreter::Run(SpuThread& t) {
    SpuContext& c = t.ctx;
    V* const g = c.gpr;
    u8* const ls = c.ls;
    u32 pc = c.pc & kLsMask & ~3u;
    u32 budget = 0;

    auto fail = [&](const char* what, u32 insn) {
        char text[160];
        std::snprintf(text, sizeof(text), "%s: 0x%08x  %s", what, insn, Disassemble(pc, insn).c_str());
        c.pc = pc;
        t.Fault(text);
    };

    for (;;) {
        if ((++budget & 0xFFF) == 0) [[unlikely]] {
            t.live_pc.store(pc, std::memory_order_relaxed);
            t.live_instructions.store(executed_, std::memory_order_relaxed);
            if (t.ShouldStop()) {
                c.pc = pc;
                return;
            }
        }
        const u32 i = FetchWord(ls, pc);
        u32 next = (pc + 4) & kLsMask;
        ++executed_;
        const u32 rt = FieldRT(i), ra = FieldRA(i), rb = FieldRB(i);

        switch (Decode(i)) {
        // ------------------------------------------------------------- control
        case SpuOp::STOP:
            c.pc = pc;
            if (!t.StopAndSignal(i & 0x3FFF)) return;
            break;
        case SpuOp::STOPD: fail("stopd (debugger breakpoint)", i); return;
        case SpuOp::LNOP:
        case SpuOp::NOP:
        case SpuOp::HBR:
        case SpuOp::HBRA:
        case SpuOp::HBRR: break;
        case SpuOp::SYNC:
        case SpuOp::DSYNC: std::atomic_thread_fence(std::memory_order_seq_cst); break;
        case SpuOp::MFSPR: g[rt] = V{}; break;
        case SpuOp::MTSPR: break;
        case SpuOp::RDCH: {
            c.pc = pc;
            u32 v = 0;
            if (!t.ReadChannel(ra, v)) return;
            SetPref(g[rt], v);
            break;
        }
        case SpuOp::RCHCNT: SetPref(g[rt], t.ChannelCount(ra)); break;
        case SpuOp::WRCH:
            c.pc = pc;
            if (!t.WriteChannel(ra, Pref(g[rt]))) return;
            break;

        // ------------------------------------------------------------- integer
        case SpuOp::A: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return a + b; }); break;
        case SpuOp::AI: { const u32 imm = static_cast<u32>(FieldI10(i)); MapWords1(g[rt], g[ra], [=](u32 a) { return a + imm; }); break; }
        case SpuOp::AH: MapHalves(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return a + b; }); break;
        case SpuOp::AHI: { const u32 imm = static_cast<u32>(FieldI10(i)); MapHalves(g[rt], g[ra], g[ra], [=](u32 a, u32) { return a + imm; }); break; }
        case SpuOp::SF: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return b - a; }); break;
        case SpuOp::SFI: { const u32 imm = static_cast<u32>(FieldI10(i)); MapWords1(g[rt], g[ra], [=](u32 a) { return imm - a; }); break; }
        case SpuOp::SFH: MapHalves(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return b - a; }); break;
        case SpuOp::SFHI: { const u32 imm = static_cast<u32>(FieldI10(i)); MapHalves(g[rt], g[ra], g[ra], [=](u32 a, u32) { return imm - a; }); break; }
        case SpuOp::CG: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return static_cast<u32>((u64{a} + b) >> 32); }); break;
        case SpuOp::BG: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return b >= a ? 1u : 0u; }); break;
        case SpuOp::ADDX: {
            const W4 qa = Words(g[ra]), qb = Words(g[rb]);
            W4 qt = Words(g[rt]);
            for (int k = 0; k < 4; ++k) qt.w[k] = qa.w[k] + qb.w[k] + (qt.w[k] & 1);
            SetWords(g[rt], qt);
            break;
        }
        case SpuOp::SFX: {
            const W4 qa = Words(g[ra]), qb = Words(g[rb]);
            W4 qt = Words(g[rt]);
            for (int k = 0; k < 4; ++k) qt.w[k] = qb.w[k] + ~qa.w[k] + (qt.w[k] & 1);
            SetWords(g[rt], qt);
            break;
        }
        case SpuOp::CGX: {
            const W4 qa = Words(g[ra]), qb = Words(g[rb]);
            W4 qt = Words(g[rt]);
            for (int k = 0; k < 4; ++k) qt.w[k] = static_cast<u32>((u64{qa.w[k]} + qb.w[k] + (qt.w[k] & 1)) >> 32);
            SetWords(g[rt], qt);
            break;
        }
        case SpuOp::BGX: {
            const W4 qa = Words(g[ra]), qb = Words(g[rb]);
            W4 qt = Words(g[rt]);
            for (int k = 0; k < 4; ++k) qt.w[k] = (qt.w[k] & 1) ? (qb.w[k] >= qa.w[k]) : (qb.w[k] > qa.w[k]);
            SetWords(g[rt], qt);
            break;
        }
        case SpuOp::MPY:
            MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return static_cast<u32>(s32{static_cast<s16>(a)} * static_cast<s16>(b)); });
            break;
        case SpuOp::MPYU: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return (a & 0xFFFF) * (b & 0xFFFF); }); break;
        case SpuOp::MPYH: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return ((a >> 16) * (b & 0xFFFF)) << 16; }); break;
        case SpuOp::MPYS:
            MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) {
                return static_cast<u32>((s32{static_cast<s16>(a)} * static_cast<s16>(b)) >> 16);
            });
            break;
        case SpuOp::MPYHH:
            MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) {
                return static_cast<u32>(s32{static_cast<s16>(a >> 16)} * static_cast<s16>(b >> 16));
            });
            break;
        case SpuOp::MPYHHU: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return (a >> 16) * (b >> 16); }); break;
        case SpuOp::MPYHHA:
        case SpuOp::MPYHHAU: {
            const bool u = Decode(i) == SpuOp::MPYHHAU;
            const W4 qa = Words(g[ra]), qb = Words(g[rb]);
            W4 qt = Words(g[rt]);
            for (int k = 0; k < 4; ++k)
                qt.w[k] += u ? (qa.w[k] >> 16) * (qb.w[k] >> 16)
                             : static_cast<u32>(s32{static_cast<s16>(qa.w[k] >> 16)} * static_cast<s16>(qb.w[k] >> 16));
            SetWords(g[rt], qt);
            break;
        }
        case SpuOp::MPYI: {
            const s32 imm = FieldI10(i);
            MapWords1(g[rt], g[ra], [=](u32 a) { return static_cast<u32>(s32{static_cast<s16>(a)} * imm); });
            break;
        }
        case SpuOp::MPYUI: {
            const u32 imm = static_cast<u32>(FieldI10(i)) & 0xFFFF;
            MapWords1(g[rt], g[ra], [=](u32 a) { return (a & 0xFFFF) * imm; });
            break;
        }
        case SpuOp::MPYA: {
            const W4 qa = Words(g[ra]), qb = Words(g[rb]), qc = Words(g[rt]);  // rt field = RC here
            W4 r;
            for (int k = 0; k < 4; ++k)
                r.w[k] = static_cast<u32>(s32{static_cast<s16>(qa.w[k])} * static_cast<s16>(qb.w[k])) + qc.w[k];
            SetWords(g[FieldRT4(i)], r);
            break;
        }
        case SpuOp::CLZ: MapWords1(g[rt], g[ra], [](u32 a) { return static_cast<u32>(std::countl_zero(a)); }); break;
        case SpuOp::CNTB: {
            V r;
            for (int k = 0; k < 16; ++k) r.bytes[k] = static_cast<u8>(std::popcount(g[ra].bytes[k]));
            g[rt] = r;
            break;
        }
        case SpuOp::ABSDB: MapBytes(g[rt], g[ra], g[rb], [](u8 a, u8 b) { return a > b ? a - b : b - a; }); break;
        case SpuOp::AVGB: MapBytes(g[rt], g[ra], g[rb], [](u8 a, u8 b) { return (u32{a} + b + 1) >> 1; }); break;
        case SpuOp::SUMB: {
            V r;
            for (int k = 0; k < 4; ++k) {
                u32 sa = 0, sb = 0;
                for (int j = 0; j < 4; ++j) {
                    sa += g[ra].bytes[4 * k + j];
                    sb += g[rb].bytes[4 * k + j];
                }
                SetHalf(r, 2 * k, static_cast<u16>(sb));
                SetHalf(r, 2 * k + 1, static_cast<u16>(sa));
            }
            g[rt] = r;
            break;
        }
        case SpuOp::XSBH: {
            V r;
            for (int k = 0; k < 8; ++k) SetHalf(r, k, static_cast<u16>(static_cast<s8>(g[ra].bytes[2 * k + 1])));
            g[rt] = r;
            break;
        }
        case SpuOp::XSHW: MapWords1(g[rt], g[ra], [](u32 a) { return static_cast<u32>(s32{static_cast<s16>(a)}); }); break;
        case SpuOp::XSWD: {
            W4 q = Words(g[ra]);
            for (int k = 0; k < 2; ++k) q.w[2 * k] = (q.w[2 * k + 1] & 0x80000000u) ? 0xFFFFFFFFu : 0;
            SetWords(g[rt], q);
            break;
        }

        // ------------------------------------------------------------- logical
        case SpuOp::AND: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return a & b; }); break;
        case SpuOp::ANDC: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return a & ~b; }); break;
        case SpuOp::OR: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return a | b; }); break;
        case SpuOp::ORC: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return a | ~b; }); break;
        case SpuOp::XOR: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return a ^ b; }); break;
        case SpuOp::NAND: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return ~(a & b); }); break;
        case SpuOp::NOR: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return ~(a | b); }); break;
        case SpuOp::EQV: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return ~(a ^ b); }); break;
        case SpuOp::ANDI: { const u32 m = static_cast<u32>(FieldI10(i)); MapWords1(g[rt], g[ra], [=](u32 a) { return a & m; }); break; }
        case SpuOp::ORI: { const u32 m = static_cast<u32>(FieldI10(i)); MapWords1(g[rt], g[ra], [=](u32 a) { return a | m; }); break; }
        case SpuOp::XORI: { const u32 m = static_cast<u32>(FieldI10(i)); MapWords1(g[rt], g[ra], [=](u32 a) { return a ^ m; }); break; }
        case SpuOp::ANDHI: { const u32 m = static_cast<u32>(FieldI10(i)) & 0xFFFF; MapHalves(g[rt], g[ra], g[ra], [=](u32 a, u32) { return a & m; }); break; }
        case SpuOp::ORHI: { const u32 m = static_cast<u32>(FieldI10(i)) & 0xFFFF; MapHalves(g[rt], g[ra], g[ra], [=](u32 a, u32) { return a | m; }); break; }
        case SpuOp::XORHI: { const u32 m = static_cast<u32>(FieldI10(i)) & 0xFFFF; MapHalves(g[rt], g[ra], g[ra], [=](u32 a, u32) { return a ^ m; }); break; }
        case SpuOp::ANDBI: { const u8 m = static_cast<u8>(FieldI10(i)); MapBytes(g[rt], g[ra], g[ra], [=](u8 a, u8) { return a & m; }); break; }
        case SpuOp::ORBI: { const u8 m = static_cast<u8>(FieldI10(i)); MapBytes(g[rt], g[ra], g[ra], [=](u8 a, u8) { return a | m; }); break; }
        case SpuOp::XORBI: { const u8 m = static_cast<u8>(FieldI10(i)); MapBytes(g[rt], g[ra], g[ra], [=](u8 a, u8) { return a ^ m; }); break; }
        case SpuOp::ORX: {
            const W4 q = Words(g[ra]);
            SetPref(g[rt], q.w[0] | q.w[1] | q.w[2] | q.w[3]);
            break;
        }
        case SpuOp::SELB: {
            const V &a = g[ra], &b = g[rb], &m = g[rt];  // rt field = RC (mask)
            V r;
            for (int k = 0; k < 16; ++k) r.bytes[k] = static_cast<u8>((m.bytes[k] & b.bytes[k]) | (~m.bytes[k] & a.bytes[k]));
            g[FieldRT4(i)] = r;
            break;
        }
        case SpuOp::SHUFB: {
            const V &a = g[ra], &b = g[rb], &m = g[rt];  // rt field = RC (pattern)
            V r;
            for (int k = 0; k < 16; ++k) {
                const u8 s = m.bytes[k];
                if ((s & 0xC0) == 0x80) r.bytes[k] = 0x00;
                else if ((s & 0xE0) == 0xC0) r.bytes[k] = 0xFF;
                else if ((s & 0xE0) == 0xE0) r.bytes[k] = 0x80;
                else r.bytes[k] = (s & 0x10) ? b.bytes[s & 0xF] : a.bytes[s & 0xF];
            }
            g[FieldRT4(i)] = r;
            break;
        }

        // ------------------------------------------------------------- compares / halts
        case SpuOp::CEQ: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return Mask(a == b); }); break;
        case SpuOp::CEQH: MapHalves(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return a == b ? 0xFFFFu : 0u; }); break;
        case SpuOp::CEQB: MapBytes(g[rt], g[ra], g[rb], [](u8 a, u8 b) { return a == b ? 0xFF : 0; }); break;
        case SpuOp::CGT: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return Mask(static_cast<s32>(a) > static_cast<s32>(b)); }); break;
        case SpuOp::CGTH: MapHalves(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return static_cast<s16>(a) > static_cast<s16>(b) ? 0xFFFFu : 0u; }); break;
        case SpuOp::CGTB: MapBytes(g[rt], g[ra], g[rb], [](u8 a, u8 b) { return static_cast<s8>(a) > static_cast<s8>(b) ? 0xFF : 0; }); break;
        case SpuOp::CLGT: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return Mask(a > b); }); break;
        case SpuOp::CLGTH: MapHalves(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return a > b ? 0xFFFFu : 0u; }); break;
        case SpuOp::CLGTB: MapBytes(g[rt], g[ra], g[rb], [](u8 a, u8 b) { return a > b ? 0xFF : 0; }); break;
        case SpuOp::CEQI: { const u32 m = static_cast<u32>(FieldI10(i)); MapWords1(g[rt], g[ra], [=](u32 a) { return Mask(a == m); }); break; }
        case SpuOp::CEQHI: { const u32 m = static_cast<u32>(FieldI10(i)) & 0xFFFF; MapHalves(g[rt], g[ra], g[ra], [=](u32 a, u32) { return a == m ? 0xFFFFu : 0u; }); break; }
        case SpuOp::CEQBI: { const u8 m = static_cast<u8>(FieldI10(i)); MapBytes(g[rt], g[ra], g[ra], [=](u8 a, u8) { return a == m ? 0xFF : 0; }); break; }
        case SpuOp::CGTI: { const s32 m = FieldI10(i); MapWords1(g[rt], g[ra], [=](u32 a) { return Mask(static_cast<s32>(a) > m); }); break; }
        case SpuOp::CGTHI: { const s16 m = static_cast<s16>(FieldI10(i)); MapHalves(g[rt], g[ra], g[ra], [=](u32 a, u32) { return static_cast<s16>(a) > m ? 0xFFFFu : 0u; }); break; }
        case SpuOp::CGTBI: { const s8 m = static_cast<s8>(FieldI10(i)); MapBytes(g[rt], g[ra], g[ra], [=](u8 a, u8) { return static_cast<s8>(a) > m ? 0xFF : 0; }); break; }
        case SpuOp::CLGTI: { const u32 m = static_cast<u32>(FieldI10(i)); MapWords1(g[rt], g[ra], [=](u32 a) { return Mask(a > m); }); break; }
        case SpuOp::CLGTHI: { const u32 m = static_cast<u32>(FieldI10(i)) & 0xFFFF; MapHalves(g[rt], g[ra], g[ra], [=](u32 a, u32) { return a > m ? 0xFFFFu : 0u; }); break; }
        case SpuOp::CLGTBI: { const u8 m = static_cast<u8>(FieldI10(i)); MapBytes(g[rt], g[ra], g[ra], [=](u8 a, u8) { return a > m ? 0xFF : 0; }); break; }
        case SpuOp::HEQ:
            if (Pref(g[ra]) == Pref(g[rb])) { fail("halt (heq)", i); return; }
            break;
        case SpuOp::HEQI:
            if (Pref(g[ra]) == static_cast<u32>(FieldI10(i))) { fail("halt (heqi)", i); return; }
            break;
        case SpuOp::HGT:
            if (static_cast<s32>(Pref(g[ra])) > static_cast<s32>(Pref(g[rb]))) { fail("halt (hgt)", i); return; }
            break;
        case SpuOp::HGTI:
            if (static_cast<s32>(Pref(g[ra])) > FieldI10(i)) { fail("halt (hgti)", i); return; }
            break;
        case SpuOp::HLGT:
            if (Pref(g[ra]) > Pref(g[rb])) { fail("halt (hlgt)", i); return; }
            break;
        case SpuOp::HLGTI:
            if (Pref(g[ra]) > static_cast<u32>(FieldI10(i))) { fail("halt (hlgti)", i); return; }
            break;

        // ------------------------------------------------------------- element shifts / rotates
        case SpuOp::SHL: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { const u32 n = b & 0x3F; return n > 31 ? 0u : a << n; }); break;
        case SpuOp::SHLI: { const u32 n = static_cast<u32>(FieldI7(i)) & 0x3F; MapWords1(g[rt], g[ra], [=](u32 a) { return n > 31 ? 0u : a << n; }); break; }
        case SpuOp::SHLH: MapHalves(g[rt], g[ra], g[rb], [](u32 a, u32 b) { const u32 n = b & 0x1F; return n > 15 ? 0u : a << n; }); break;
        case SpuOp::SHLHI: { const u32 n = static_cast<u32>(FieldI7(i)) & 0x1F; MapHalves(g[rt], g[ra], g[ra], [=](u32 a, u32) { return n > 15 ? 0u : a << n; }); break; }
        case SpuOp::ROT: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return std::rotl(a, static_cast<int>(b & 0x1F)); }); break;
        case SpuOp::ROTI: { const int n = FieldI7(i) & 0x1F; MapWords1(g[rt], g[ra], [=](u32 a) { return std::rotl(a, n); }); break; }
        case SpuOp::ROTH: MapHalves(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return std::rotl(static_cast<u16>(a), static_cast<int>(b & 0xF)); }); break;
        case SpuOp::ROTHI: { const int n = FieldI7(i) & 0xF; MapHalves(g[rt], g[ra], g[ra], [=](u32 a, u32) { return std::rotl(static_cast<u16>(a), n); }); break; }
        case SpuOp::ROTM: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { const u32 n = (0 - b) & 0x3F; return n > 31 ? 0u : a >> n; }); break;
        case SpuOp::ROTMI: { const u32 n = (0 - static_cast<u32>(FieldI7(i))) & 0x3F; MapWords1(g[rt], g[ra], [=](u32 a) { return n > 31 ? 0u : a >> n; }); break; }
        case SpuOp::ROTMA:
            MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) {
                const u32 n = (0 - b) & 0x3F;
                return static_cast<u32>(static_cast<s32>(a) >> (n > 31 ? 31 : n));
            });
            break;
        case SpuOp::ROTMAI: {
            const u32 n = (0 - static_cast<u32>(FieldI7(i))) & 0x3F;
            MapWords1(g[rt], g[ra], [=](u32 a) { return static_cast<u32>(static_cast<s32>(a) >> (n > 31 ? 31 : n)); });
            break;
        }
        case SpuOp::ROTHM: MapHalves(g[rt], g[ra], g[rb], [](u32 a, u32 b) { const u32 n = (0 - b) & 0x1F; return n > 15 ? 0u : a >> n; }); break;
        case SpuOp::ROTHMI: { const u32 n = (0 - static_cast<u32>(FieldI7(i))) & 0x1F; MapHalves(g[rt], g[ra], g[ra], [=](u32 a, u32) { return n > 15 ? 0u : a >> n; }); break; }
        case SpuOp::ROTMAH:
            MapHalves(g[rt], g[ra], g[rb], [](u32 a, u32 b) {
                const u32 n = (0 - b) & 0x1F;
                return static_cast<u32>(static_cast<s16>(a) >> (n > 15 ? 15 : n));
            });
            break;
        case SpuOp::ROTMAHI: {
            const u32 n = (0 - static_cast<u32>(FieldI7(i))) & 0x1F;
            MapHalves(g[rt], g[ra], g[ra], [=](u32 a, u32) { return static_cast<u32>(static_cast<s16>(a) >> (n > 15 ? 15 : n)); });
            break;
        }

        // ------------------------------------------------------------- quadword shifts / rotates
        case SpuOp::SHLQBI: g[rt] = ShiftLeftBits(g[ra], Pref(g[rb]) & 7); break;
        case SpuOp::SHLQBII: g[rt] = ShiftLeftBits(g[ra], static_cast<u32>(FieldI7(i)) & 7); break;
        case SpuOp::ROTQBI: g[rt] = RotateLeftBits(g[ra], Pref(g[rb]) & 7); break;
        case SpuOp::ROTQBII: g[rt] = RotateLeftBits(g[ra], static_cast<u32>(FieldI7(i)) & 7); break;
        case SpuOp::ROTQMBI: g[rt] = ShiftRightBits(g[ra], (0 - Pref(g[rb])) & 7); break;
        case SpuOp::ROTQMBII: g[rt] = ShiftRightBits(g[ra], (0 - static_cast<u32>(FieldI7(i))) & 7); break;
        case SpuOp::SHLQBY: { const u32 n = Pref(g[rb]) & 0x1F; g[rt] = n > 15 ? V{} : ShiftLeftBytes(g[ra], n); break; }
        case SpuOp::SHLQBYI: { const u32 n = static_cast<u32>(FieldI7(i)) & 0x1F; g[rt] = n > 15 ? V{} : ShiftLeftBytes(g[ra], n); break; }
        case SpuOp::SHLQBYBI: { const u32 n = (Pref(g[rb]) >> 3) & 0x1F; g[rt] = n > 15 ? V{} : ShiftLeftBytes(g[ra], n); break; }
        case SpuOp::ROTQBY: g[rt] = RotateLeftBytes(g[ra], Pref(g[rb]) & 0xF); break;
        case SpuOp::ROTQBYI: g[rt] = RotateLeftBytes(g[ra], static_cast<u32>(FieldI7(i)) & 0xF); break;
        case SpuOp::ROTQBYBI: g[rt] = RotateLeftBytes(g[ra], (Pref(g[rb]) >> 3) & 0xF); break;
        case SpuOp::ROTQMBY: { const u32 n = (0 - Pref(g[rb])) & 0x1F; g[rt] = n > 15 ? V{} : ShiftRightBytes(g[ra], n); break; }
        case SpuOp::ROTQMBYI: { const u32 n = (0 - static_cast<u32>(FieldI7(i))) & 0x1F; g[rt] = n > 15 ? V{} : ShiftRightBytes(g[ra], n); break; }
        case SpuOp::ROTQMBYBI: { const u32 n = (0 - (Pref(g[rb]) >> 3)) & 0x1F; g[rt] = n > 15 ? V{} : ShiftRightBytes(g[ra], n); break; }

        // ------------------------------------------------------------- branches
        case SpuOp::BR: next = (pc + (static_cast<u32>(FieldS16(i)) << 2)) & kLsMask; break;
        case SpuOp::BRA: next = (static_cast<u32>(FieldS16(i)) << 2) & kLsMask; break;
        case SpuOp::BRSL:
            SetPref(g[rt], next);
            next = (pc + (static_cast<u32>(FieldS16(i)) << 2)) & kLsMask;
            break;
        case SpuOp::BRASL:
            SetPref(g[rt], next);
            next = (static_cast<u32>(FieldS16(i)) << 2) & kLsMask;
            break;
        case SpuOp::BRZ: if (Pref(g[rt]) == 0) next = (pc + (static_cast<u32>(FieldS16(i)) << 2)) & kLsMask; break;
        case SpuOp::BRNZ: if (Pref(g[rt]) != 0) next = (pc + (static_cast<u32>(FieldS16(i)) << 2)) & kLsMask; break;
        case SpuOp::BRHZ: if ((Pref(g[rt]) & 0xFFFF) == 0) next = (pc + (static_cast<u32>(FieldS16(i)) << 2)) & kLsMask; break;
        case SpuOp::BRHNZ: if ((Pref(g[rt]) & 0xFFFF) != 0) next = (pc + (static_cast<u32>(FieldS16(i)) << 2)) & kLsMask; break;
        case SpuOp::BI: next = Pref(g[ra]) & kLsMask & ~3u; break;
        case SpuOp::BISL: {
            const u32 target = Pref(g[ra]) & kLsMask & ~3u;
            SetPref(g[rt], next);
            next = target;
            break;
        }
        case SpuOp::BISLED:
            if (t.ChannelCount(SPU_RdEventStat)) {
                const u32 target = Pref(g[ra]) & kLsMask & ~3u;
                SetPref(g[rt], next);
                next = target;
            }
            break;
        case SpuOp::IRET: next = c.srr0 & kLsMask & ~3u; break;
        case SpuOp::BIZ: if (Pref(g[rt]) == 0) next = Pref(g[ra]) & kLsMask & ~3u; break;
        case SpuOp::BINZ: if (Pref(g[rt]) != 0) next = Pref(g[ra]) & kLsMask & ~3u; break;
        case SpuOp::BIHZ: if ((Pref(g[rt]) & 0xFFFF) == 0) next = Pref(g[ra]) & kLsMask & ~3u; break;
        case SpuOp::BIHNZ: if ((Pref(g[rt]) & 0xFFFF) != 0) next = Pref(g[ra]) & kLsMask & ~3u; break;

        // ------------------------------------------------------------- masks / gather
        case SpuOp::FSMBI:
        case SpuOp::FSMB: {
            const u32 bits = Decode(i) == SpuOp::FSMBI ? FieldI16(i) : Pref(g[ra]) & 0xFFFF;
            V r;
            for (int k = 0; k < 16; ++k) r.bytes[k] = ((bits >> (15 - k)) & 1) ? 0xFF : 0;
            g[rt] = r;
            break;
        }
        case SpuOp::FSMH: {
            const u32 bits = Pref(g[ra]) & 0xFF;
            V r;
            for (int k = 0; k < 8; ++k) SetHalf(r, k, ((bits >> (7 - k)) & 1) ? 0xFFFF : 0);
            g[rt] = r;
            break;
        }
        case SpuOp::FSM: {
            const u32 bits = Pref(g[ra]) & 0xF;
            W4 r;
            for (int k = 0; k < 4; ++k) r.w[k] = Mask((bits >> (3 - k)) & 1);
            SetWords(g[rt], r);
            break;
        }
        case SpuOp::GB: {
            const W4 q = Words(g[ra]);
            u32 r = 0;
            for (int k = 0; k < 4; ++k) r = r << 1 | (q.w[k] & 1);
            SetPref(g[rt], r);
            break;
        }
        case SpuOp::GBH: {
            u32 r = 0;
            for (int k = 0; k < 8; ++k) r = r << 1 | (Half(g[ra], k) & 1);
            SetPref(g[rt], r);
            break;
        }
        case SpuOp::GBB: {
            u32 r = 0;
            for (int k = 0; k < 16; ++k) r = r << 1 | (g[ra].bytes[k] & 1);
            SetPref(g[rt], r);
            break;
        }

        // ------------------------------------------------------------- local store
        case SpuOp::LQD: vm::GuestCopy(g[rt].bytes, ls + ((Pref(g[ra]) + (static_cast<u32>(FieldI10(i)) << 4)) & kLsMask & ~15u), 16); break;
        case SpuOp::LQX: vm::GuestCopy(g[rt].bytes, ls + ((Pref(g[ra]) + Pref(g[rb])) & kLsMask & ~15u), 16); break;
        case SpuOp::LQA: vm::GuestCopy(g[rt].bytes, ls + ((static_cast<u32>(FieldS16(i)) << 2) & kLsMask & ~15u), 16); break;
        case SpuOp::LQR: vm::GuestCopy(g[rt].bytes, ls + ((pc + (static_cast<u32>(FieldS16(i)) << 2)) & kLsMask & ~15u), 16); break;
        case SpuOp::STQD: vm::GuestCopy(ls + ((Pref(g[ra]) + (static_cast<u32>(FieldI10(i)) << 4)) & kLsMask & ~15u), g[rt].bytes, 16); break;
        case SpuOp::STQX: vm::GuestCopy(ls + ((Pref(g[ra]) + Pref(g[rb])) & kLsMask & ~15u), g[rt].bytes, 16); break;
        case SpuOp::STQA: vm::GuestCopy(ls + ((static_cast<u32>(FieldS16(i)) << 2) & kLsMask & ~15u), g[rt].bytes, 16); break;
        case SpuOp::STQR: vm::GuestCopy(ls + ((pc + (static_cast<u32>(FieldS16(i)) << 2)) & kLsMask & ~15u), g[rt].bytes, 16); break;
        case SpuOp::CBD: g[rt] = InsertionControl(Pref(g[ra]) + static_cast<u32>(FieldI7(i)), 1); break;
        case SpuOp::CHD: g[rt] = InsertionControl(Pref(g[ra]) + static_cast<u32>(FieldI7(i)), 2); break;
        case SpuOp::CWD: g[rt] = InsertionControl(Pref(g[ra]) + static_cast<u32>(FieldI7(i)), 4); break;
        case SpuOp::CDD: g[rt] = InsertionControl(Pref(g[ra]) + static_cast<u32>(FieldI7(i)), 8); break;
        case SpuOp::CBX: g[rt] = InsertionControl(Pref(g[ra]) + Pref(g[rb]), 1); break;
        case SpuOp::CHX: g[rt] = InsertionControl(Pref(g[ra]) + Pref(g[rb]), 2); break;
        case SpuOp::CWX: g[rt] = InsertionControl(Pref(g[ra]) + Pref(g[rb]), 4); break;
        case SpuOp::CDX: g[rt] = InsertionControl(Pref(g[ra]) + Pref(g[rb]), 8); break;

        // ------------------------------------------------------------- immediate loads
        case SpuOp::IL: { const u32 v = static_cast<u32>(FieldS16(i)); SetWords(g[rt], {{v, v, v, v}}); break; }
        case SpuOp::ILHU: { const u32 v = FieldI16(i) << 16; SetWords(g[rt], {{v, v, v, v}}); break; }
        case SpuOp::ILH: { const u32 v = FieldI16(i) * 0x10001u; SetWords(g[rt], {{v, v, v, v}}); break; }
        case SpuOp::ILA: { const u32 v = FieldI18(i); SetWords(g[rt], {{v, v, v, v}}); break; }
        case SpuOp::IOHL: { const u32 v = FieldI16(i); MapWords1(g[rt], g[rt], [=](u32 a) { return a | v; }); break; }

        // ------------------------------------------------------------- single precision
        case SpuOp::FA: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return Fbits(F(a) + F(b)); }); break;
        case SpuOp::FS: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return Fbits(F(a) - F(b)); }); break;
        case SpuOp::FM: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return Fbits(F(a) * F(b)); }); break;
        case SpuOp::FMA:
        case SpuOp::FNMS:
        case SpuOp::FMS: {
            const SpuOp op = Decode(i);
            const W4 qa = Words(g[ra]), qb = Words(g[rb]), qc = Words(g[rt]);  // rt field = RC
            W4 r;
            for (int k = 0; k < 4; ++k) {
                const float a = F(qa.w[k]), b = F(qb.w[k]), cc = F(qc.w[k]);
                r.w[k] = Fbits(op == SpuOp::FMA ? std::fma(a, b, cc) : op == SpuOp::FMS ? std::fma(a, b, -cc) : std::fma(-a, b, cc));
            }
            SetWords(g[FieldRT4(i)], r);
            break;
        }
        case SpuOp::FCEQ: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return Mask(F(a) == F(b)); }); break;
        case SpuOp::FCMEQ: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return Mask(std::fabs(F(a)) == std::fabs(F(b))); }); break;
        case SpuOp::FCGT: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return Mask(F(a) > F(b)); }); break;
        case SpuOp::FCMGT: MapWords(g[rt], g[ra], g[rb], [](u32 a, u32 b) { return Mask(std::fabs(F(a)) > std::fabs(F(b))); }); break;
        case SpuOp::FREST: MapWords1(g[rt], g[ra], [](u32 a) { return Fbits(1.0f / F(a)); }); break;
        case SpuOp::FRSQEST: MapWords1(g[rt], g[ra], [](u32 a) { return Fbits(1.0f / std::sqrt(std::fabs(F(a)))); }); break;
        case SpuOp::FI: g[rt] = g[rb]; break;
        case SpuOp::CFLTS: {
            const int scale = 173 - static_cast<int>(FieldI8(i));
            MapWords1(g[rt], g[ra], [=](u32 a) { return ToSigned(std::ldexp(static_cast<double>(F(a)), scale)); });
            break;
        }
        case SpuOp::CFLTU: {
            const int scale = 173 - static_cast<int>(FieldI8(i));
            MapWords1(g[rt], g[ra], [=](u32 a) { return ToUnsigned(std::ldexp(static_cast<double>(F(a)), scale)); });
            break;
        }
        case SpuOp::CSFLT: {
            const int scale = 155 - static_cast<int>(FieldI8(i));
            MapWords1(g[rt], g[ra], [=](u32 a) {
                return Fbits(static_cast<float>(std::ldexp(static_cast<double>(static_cast<s32>(a)), -scale)));
            });
            break;
        }
        case SpuOp::CUFLT: {
            const int scale = 155 - static_cast<int>(FieldI8(i));
            MapWords1(g[rt], g[ra], [=](u32 a) { return Fbits(static_cast<float>(std::ldexp(static_cast<double>(a), -scale))); });
            break;
        }
        case SpuOp::FSCRRD: g[rt] = V{}; break;
        case SpuOp::FSCRWR: break;

        // ------------------------------------------------------------- double precision
        case SpuOp::DFA:
        case SpuOp::DFS:
        case SpuOp::DFM:
        case SpuOp::DFMA:
        case SpuOp::DFMS:
        case SpuOp::DFNMS:
        case SpuOp::DFNMA:
        case SpuOp::DFCEQ:
        case SpuOp::DFCMEQ:
        case SpuOp::DFCGT:
        case SpuOp::DFCMGT: {
            const SpuOp op = Decode(i);
            const W4 qa = Words(g[ra]), qb = Words(g[rb]), qt = Words(g[rt]);
            W4 r;
            for (int k = 0; k < 2; ++k) {
                const double a = D(Dword(qa, k)), b = D(Dword(qb, k)), tt = D(Dword(qt, k));
                u64 v = 0;
                switch (op) {
                case SpuOp::DFA: v = Dbits(a + b); break;
                case SpuOp::DFS: v = Dbits(a - b); break;
                case SpuOp::DFM: v = Dbits(a * b); break;
                case SpuOp::DFMA: v = Dbits(std::fma(a, b, tt)); break;
                case SpuOp::DFMS: v = Dbits(std::fma(a, b, -tt)); break;
                case SpuOp::DFNMS: v = Dbits(std::fma(-a, b, tt)); break;
                case SpuOp::DFNMA: v = Dbits(-std::fma(a, b, tt)); break;
                case SpuOp::DFCEQ: v = a == b ? ~u64{0} : 0; break;
                case SpuOp::DFCMEQ: v = std::fabs(a) == std::fabs(b) ? ~u64{0} : 0; break;
                case SpuOp::DFCGT: v = a > b ? ~u64{0} : 0; break;
                default: v = std::fabs(a) > std::fabs(b) ? ~u64{0} : 0; break;
                }
                SetDword(r, k, v);
            }
            SetWords(g[rt], r);
            break;
        }
        case SpuOp::DFTSV: {
            const u32 mask = static_cast<u32>(FieldI7(i)) & 0x7F;
            const W4 qa = Words(g[ra]);
            W4 r;
            for (int k = 0; k < 2; ++k) {
                const double a = D(Dword(qa, k));
                const bool neg = std::signbit(a);
                bool hit = false;
                if ((mask & 0x40) && std::isnan(a)) hit = true;
                if ((mask & 0x20) && std::isinf(a) && !neg) hit = true;
                if ((mask & 0x10) && std::isinf(a) && neg) hit = true;
                if ((mask & 0x08) && a == 0.0 && !neg) hit = true;
                if ((mask & 0x04) && a == 0.0 && neg) hit = true;
                if ((mask & 0x02) && std::fpclassify(a) == FP_SUBNORMAL && !neg) hit = true;
                if ((mask & 0x01) && std::fpclassify(a) == FP_SUBNORMAL && neg) hit = true;
                SetDword(r, k, hit ? ~u64{0} : 0);
            }
            SetWords(g[rt], r);
            break;
        }
        case SpuOp::FESD: {
            const W4 qa = Words(g[ra]);
            W4 r;
            for (int k = 0; k < 2; ++k) SetDword(r, k, Dbits(static_cast<double>(F(qa.w[2 * k]))));
            SetWords(g[rt], r);
            break;
        }
        case SpuOp::FRDS: {
            const W4 qa = Words(g[ra]);
            W4 r{};
            for (int k = 0; k < 2; ++k) r.w[2 * k] = Fbits(static_cast<float>(D(Dword(qa, k))));
            SetWords(g[rt], r);
            break;
        }

        case SpuOp::Invalid:
        default: fail("unknown SPU instruction", i); return;
        }
        pc = next;
    }
}

}  // namespace cell::spu
