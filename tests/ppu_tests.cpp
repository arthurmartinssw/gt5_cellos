// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/ppu_tests.cpp
//
// Instruction-level tests for the PPU interpreter. Programs are assembled with a tiny encoder
// that is itself checked against well-known real encodings (as produced by ppu-lv2 GCC), so
// a misunderstood field layout cannot hide behind a matching misunderstanding in the decoder.
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hle/fnid.h"
#include "ppu_asm.h"
#include "test_machine.h"
#include "hle/hle_registry.h"
#include "hle/sys_prx_for_user.h"
#include "lv2/sys_memory.h"
#include "lv2/sys_process.h"
#include "lv2/syscall_table.h"
#include "memory/guest_memory.h"
#include "ppu/ppu_decoder.h"
#include "ppu/ppu_disasm.h"
#include "ppu/ppu_interpreter.h"

using namespace cell;

// Encodings of real instructions, as disassembled from PS3 code.
static void TestEncoder() {
    std::printf("-- encoder vs. real encodings\n");
    using namespace as;
    CHECK_EQ(mflr(0), 0x7C0802A6);
    CHECK_EQ(mtlr(0), 0x7C0803A6);
    CHECK_EQ(mtctr(0), 0x7C0903A6);
    CHECK_EQ(stdu(1, -128, 1), 0xF821FF81);
    CHECK_EQ(std_(0, 144, 1), 0xF8010090);
    CHECK_EQ(ld(0, 144, 1), 0xE8010090);
    CHECK_EQ(addi(1, 1, 128), 0x38210080);
    CHECK_EQ(li(3, 0), 0x38600000);
    CHECK_EQ(rldicl(3, 3, 0, 32), 0x78630020);  // clrldi r3,r3,32
    CHECK_EQ(rldicr(3, 3, 32, 31), 0x786307C6); // sldi r3,r3,32
    CHECK_EQ(mr(31, 3), 0x7C7F1B78);
    CHECK_EQ(cmpwi(7, 3, 0), 0x2F830000);
    CHECK_EQ(beq(7, 8), 0x419E0008);
    CHECK_EQ(lwz(0, 0, 12), 0x800C0000);
    CHECK_EQ(lwz(2, 4, 12), 0x804C0004);
    CHECK_EQ(std_(2, 40, 1), 0xF8410028);
    CHECK_EQ(rlwinm(3, 3, 0, 16, 31), 0x5463043E);  // clrlwi r3,r3,16
    CHECK_EQ(srawi(3, 3, 2), 0x7C631670);
    CHECK_EQ(extsw(3, 3), 0x7C6307B4);
    CHECK_EQ(X(0, 0, 9, 150, 1), 0x7C00492D);  // stwcx. r0,0,r9
    CHECK_EQ(X(0, 0, 9, 20), 0x7C004828);      // lwarx r0,0,r9

    // Decoder spot checks on the same words.
    CHECK(ppu::Decode(0x7C0802A6) == ppu::PpuInstr::MFSPR);
    CHECK(ppu::Decode(0xF821FF81) == ppu::PpuInstr::STDU);
    CHECK(ppu::Decode(0x78630020) == ppu::PpuInstr::RLDICL);
    CHECK(ppu::Decode(0x786307C6) == ppu::PpuInstr::RLDICR);
    CHECK(ppu::Decode(0x7C631670) == ppu::PpuInstr::SRAWI);
    CHECK(ppu::Decode(0x7C00492D) == ppu::PpuInstr::STWCX);
    CHECK(ppu::Decode(0x4E800421) == ppu::PpuInstr::BCCTR);
    CHECK(ppu::Decode(as::XO(3, 4, 5, 266, 1, 1)) == ppu::PpuInstr::ADD);  // addo.
    CHECK(ppu::Decode(as::sradi(3, 4, 40)) == ppu::PpuInstr::SRADI);       // sh5 = 1
    CHECK(ppu::Decode(0x00000000) == ppu::PpuInstr::UNK);
}

static void TestFnid() {
    std::printf("-- FNID computation\n");
    CHECK_EQ(hle::ComputeFnid("sys_initialize_tls"), 0x744680A2);
    CHECK_EQ(hle::ComputeFnid("sys_process_exit"), 0xE6F2C1E7);
    CHECK_EQ(hle::ComputeFnid("sys_game_process_exitspawn2"), 0x67F9FEDB);
    CHECK_EQ(hle::ComputeFnid("sys_lwmutex_lock"), 0x1573DC3F);
    const char* name = hle::LookupFunctionName(0xFC52A7A9);
    CHECK(name && std::string(name) == "sys_game_process_exitspawn");
}

static bool Exited(const ppu::StopInfo& s) {
    if (s.reason != ppu::StopReason::Exited) {
        std::fprintf(stderr, "   stopped: %s at 0x%08x (0x%08x) %s\n", ppu::StopReasonName(s.reason), s.pc, s.instruction,
                     s.detail.c_str());
        return false;
    }
    return true;
}

// =======================================================================================
// Tests
// =======================================================================================
static void TestIntegerArithmetic() {
    std::printf("-- integer arithmetic, carry, overflow\n");
    using namespace as;
    Machine m;
    const auto s = m.Run({
        li(3, -1),                    // r3 = 0xFFFF...FFFF
        li(4, 1),
        XO(5, 3, 4, 10),              // addc r5 = r3 + r4 = 0, CA = 1
        mfxer(6),                     // r6 = XER (CA set)
        li(7, 0), li(8, 0),
        XO(9, 7, 8, 138),             // adde r9 = 0 + 0 + CA = 1
        li(10, 5), li(11, 3),
        XO(12, 11, 10, 8),            // subfc r12 = r10 - r11 = 2, CA = 1 (no borrow)
        XO(13, 10, 11, 8),            // subfc r13 = r11 - r10 = -2, CA = 0
        mfxer(14),
        lis(15, 0x7FFF), ori(15, 15, 0xFFFF),  // r15 = 0x7FFFFFFF
        li(16, 2),
        XO(17, 15, 16, 235, 1),       // mullwo r17 = 0x7FFFFFFF * 2 -> OV, SO
        mfxer(18),
        li(19, -7), li(20, 2),
        XO(21, 19, 20, 491),          // divw r21 = -7 / 2 = -3 (zero-extended)
        XO(22, 19, 20, 459),          // divwu r22 = 0xFFFFFFF9 / 2
        XO(23, 19, 20, 489),          // divd r23 = -7 / 2 = -3
        XO(24, 3, 3, 9),              // mulhdu r24 = high(0xFFFF..^2) = 0xFFFF...FFFE
        XO(25, 19, 0, 104),           // neg r25 = 7
        D(7, 26, 19, 3),              // mulli r26 = -21
        D(8, 27, 10, 100),            // subfic r27 = 100 - 5 = 95, CA = 1
        blr(),
    });
    CHECK(Exited(s));
    const auto& c = m.ctx;
    CHECK_EQ(c.gpr[5], 0);
    CHECK(c.gpr[6] & 0x20000000);
    CHECK_EQ(c.gpr[9], 1);
    CHECK_EQ(c.gpr[12], 2);
    CHECK_EQ(c.gpr[13], static_cast<u64>(-2));
    CHECK(!(c.gpr[14] & 0x20000000));
    CHECK_EQ(c.gpr[17], 0xFFFFFFFEull);
    CHECK((c.gpr[18] & 0xC0000000) == 0xC0000000);  // OV and SO
    CHECK_EQ(c.gpr[21], 0xFFFFFFFDull);
    CHECK_EQ(c.gpr[22], 0x7FFFFFFCull);
    CHECK_EQ(c.gpr[23], static_cast<u64>(-3));
    CHECK_EQ(c.gpr[24], 0xFFFFFFFFFFFFFFFEull);
    CHECK_EQ(c.gpr[25], 7);
    CHECK_EQ(c.gpr[26], static_cast<u64>(-21));
    CHECK_EQ(c.gpr[27], 95);
    CHECK(c.xer & 0x20000000);
}

static void TestRotatesShifts() {
    std::printf("-- rotates, masks, shifts\n");
    using namespace as;
    Machine m;
    const auto s = m.Run({
        lis(3, 0x1234), ori(3, 3, 0x5678),          // r3 = 0x12345678
        rlwinm(4, 3, 0, 16, 31),                    // clrlwi 16 -> 0x5678
        rlwinm(5, 3, 8, 0, 31),                     // rotlwi 8 -> 0x34567812
        rlwinm(6, 3, 0, 28, 3),                     // wrapping mask, 64-bit mode quirk
        li(7, -1), rlwimi(7, 3, 16, 0, 15),         // insert: 0x5678FFFF (+ upper bits kept)
        rldicr(8, 3, 32, 31),                       // sldi 32
        li(9, -1), rldicl(9, 9, 0, 32),             // clrldi 32 -> 0xFFFFFFFF
        rldic(10, 3, 4, 28),                        // (r3 << 4) & MASK(28, 59)
        li(11, 0), rldimi(11, 3, 32, 0),            // insert r3 into the high word
        li(12, -64), srawi(13, 12, 3),              // -64 >> 3 = -8, CA = 0 (no 1s lost)
        li(14, -63), srawi(15, 14, 3),              // -63 >> 3 = -8, CA = 1
        mfxer(16),
        li(17, 1), li(18, 32), X(17, 19, 18, 24),   // slw r19 = r17 << 32 = 0
        li(20, -1), li(21, 40), X(20, 22, 21, 539), // srd r22 = 0xFFFF.. >> 40
        sradi(23, 12, 40),                          // -64 >> 40 = -1
        X(3, 24, 0, 26),                            // cntlzw r24 = 3
        extsw(25, 7),
        blr(),
    });
    CHECK(Exited(s));
    const auto& c = m.ctx;
    CHECK_EQ(c.gpr[4], 0x5678);
    CHECK_EQ(c.gpr[5], 0x34567812);
    CHECK_EQ(c.gpr[6], 0x1234567810000008ull);
    CHECK_EQ(static_cast<u32>(c.gpr[7]), 0x5678FFFFu);
    CHECK_EQ(c.gpr[8], 0x1234567800000000ull);
    CHECK_EQ(c.gpr[9], 0xFFFFFFFFull);
    CHECK_EQ(c.gpr[10], (0x12345678ull << 4) & 0x0000000FFFFFFFF0ull);
    CHECK_EQ(c.gpr[11], 0x1234567800000000ull);
    CHECK_EQ(c.gpr[13], static_cast<u64>(-8));
    CHECK_EQ(c.gpr[15], static_cast<u64>(-8));
    CHECK(c.gpr[16] & 0x20000000);
    CHECK_EQ(c.gpr[19], 0);
    CHECK_EQ(c.gpr[22], 0xFFFFFFull);
    CHECK_EQ(c.gpr[23], static_cast<u64>(-1));
    CHECK_EQ(c.gpr[24], 3);
    CHECK_EQ(c.gpr[25], 0x5678FFFFull);  // r7 = 0xFFFFFFFF5678FFFF; low word positive
}

static void TestBranchesAndCalls() {
    std::printf("-- branches, CR, CTR loops, calls with stack frames\n");
    using namespace as;
    Machine m;
    // sum = 1 + ... + 10 with cmpwi/bne; then 5 iterations of bdnz; then a call to a leaf
    // function through bl that builds a frame (stdu/mflr/std ... ld/mtlr/addi/blr).
    const auto s = m.Run({
        /* 0x00 */ li(3, 0), li(4, 1),
        /* 0x08 */ XO(3, 3, 4, 266),            // loop: sum += i
        /* 0x0C */ addi(4, 4, 1),
        /* 0x10 */ cmpwi(7, 4, 11),
        /* 0x14 */ bne(7, -12),                  // -> 0x08
        /* 0x18 */ li(5, 5), mtctr(5), li(6, 0),
        /* 0x24 */ addi(6, 6, 2),
        /* 0x28 */ bdnz(-4),                     // -> 0x24
        /* 0x2C */ mflr(31),                     // keep the exit LR
        /* 0x30 */ bl(0x14),                     // -> 0x44
        /* 0x34 */ mtlr(31),
        /* 0x38 */ X(9, 0, 0, 19),               // mfcr r9
        /* 0x3C */ cmpdi(0, 3, 55),
        /* 0x40 */ blr(),
        /* 0x44 */ stdu(1, -128, 1),             // leaf with frame
        /* 0x48 */ mflr(0), std_(0, 144, 1),
        /* 0x50 */ li(7, 42),
        /* 0x54 */ ld(0, 144, 1), mtlr(0),
        /* 0x5C */ addi(1, 1, 128),
        /* 0x60 */ blr(),
    });
    CHECK(Exited(s));
    const auto& c = m.ctx;
    CHECK_EQ(c.gpr[3], 55);
    CHECK_EQ(c.gpr[6], 10);
    CHECK_EQ(c.ctr, 0);
    CHECK_EQ(c.gpr[7], 42);
    CHECK_EQ(c.gpr[1], m.stack_top - 0x70);  // frame popped
    CHECK_EQ((c.gpr[9] >> 0) & 0xF, 2);       // cr7 = EQ from the last cmpwi (4 == 11)
    CHECK_EQ(c.cr >> 28, 2);                  // cr0 = EQ (r3 == 55)
}

static void TestLoadStore() {
    std::printf("-- loads/stores, big-endian layout, update forms, lmw/stmw, byte-reversed\n");
    using namespace as;
    Machine m;
    const auto s = m.Run({
        lis(9, 2),                                 // r9 = 0x20000 (data)
        lis(3, 0x1122), ori(3, 3, 0x3344),
        stw(3, 0, 9),                              // memory: 11 22 33 44
        lbz(4, 0, 9), lbz(5, 3, 9),
        lhz(6, 2, 9),
        li(7, -2), sth(7, 8, 9), lha(8, 8, 9), lhz(10, 8, 9),
        lis(11, -0x8000), std_(11, 16, 9), ld(12, 16, 9),
        lwa(13, 20, 9),                            // low word 0x80000000, sign-extended
        addi(14, 9, 32), li(15, 7), stwu(15, 8, 14), // r14 += 8
        lwzu(16, 0, 14),
        li(28, 28), li(29, 29), li(30, 30), li(31, 31),
        stmw(28, 64, 9), li(28, 0), li(31, 0), lmw(28, 64, 9),
        li(17, 0), X(18, 9, 17, 534),              // lwbrx r18 from 0x20000 -> 0x44332211
        stb(4, 100, 9), lbz(19, 100, 9),
        blr(),
    });
    CHECK(Exited(s));
    const auto& c = m.ctx;
    const u8* mem = m.memory.shadow() + kData;
    CHECK(mem[0] == 0x11 && mem[1] == 0x22 && mem[2] == 0x33 && mem[3] == 0x44);
    CHECK_EQ(c.gpr[4], 0x11);
    CHECK_EQ(c.gpr[5], 0x44);
    CHECK_EQ(c.gpr[6], 0x3344);
    CHECK_EQ(c.gpr[8], static_cast<u64>(-2));
    CHECK_EQ(c.gpr[10], 0xFFFE);
    CHECK_EQ(c.gpr[12], 0xFFFFFFFF80000000ull);
    CHECK_EQ(c.gpr[13], 0xFFFFFFFF80000000ull);
    CHECK_EQ(c.gpr[14], kData + 40);
    CHECK_EQ(c.gpr[16], 7);
    CHECK(c.gpr[28] == 28 && c.gpr[31] == 31);
    CHECK_EQ(c.gpr[18], 0x44332211);
    CHECK_EQ(c.gpr[19], 0x11);
}

static void TestAtomics() {
    std::printf("-- lwarx/stwcx. reservations\n");
    using namespace as;
    Machine m;
    const auto s = m.Run({
        lis(9, 2), li(3, 10), stw(3, 0, 9),
        X(4, 0, 9, 20),                  // lwarx r4
        addi(4, 4, 1),
        X(4, 0, 9, 150, 1),              // stwcx. -> success
        X(0, 0, 0, 19), mr(20, 0),       // mfcr -> r20
        X(4, 0, 9, 150, 1),              // stwcx. again, no reservation -> fail
        X(0, 0, 0, 19), mr(21, 0),
        lwz(22, 0, 9),
        blr(),
    });
    CHECK(Exited(s));
    CHECK_EQ((m.ctx.gpr[20] >> 28) & 2, 2);  // cr0.EQ = success
    CHECK_EQ((m.ctx.gpr[21] >> 28) & 2, 0);
    CHECK_EQ(m.ctx.gpr[22], 11);
}

static void TestFloatingPointClean() {
    std::printf("-- FPU (arithmetic, conversions, compares)\n");
    using namespace as;
    Machine m;
    m.memory.Write<u64>(kData, std::bit_cast<u64>(1.5));
    m.memory.Write<u32>(kData + 8, std::bit_cast<u32>(2.25f));
    m.memory.Write<u64>(kData + 16, std::bit_cast<u64>(-7.75));
    const u32 fcmpu_cr1_f1_f2 = 63u << 26 | (1u << 23) | 1u << 16 | 2u << 11;  // xo 0
    const auto s = m.Run({
        lis(9, 2), li(10, 48),
        lfd(1, 0, 9), lfs(2, 8, 9), lfd(3, 16, 9),
        A(63, 4, 1, 2, 0, 21),       // fadd  f4 = 3.75
        A(63, 5, 1, 0, 2, 25),       // fmul  f5 = 3.375
        A(63, 6, 1, 3, 2, 29),       // fmadd f6 = 1.5*2.25 - 7.75 = -4.375
        A(59, 7, 1, 2, 0, 21),       // fadds f7 = 3.75
        A(63, 8, 0, 3, 0, 15),       // fctiwz f8 = -7
        X(8, 9, 10, 983),            // stfiwx f8 -> [0x20030]
        stfd(4, 32, 9), stfs(5, 40, 9),
        fcmpu_cr1_f1_f2,             // 1.5 < 2.25 -> cr1 = LT
        A(63, 11, 0, 3, 0, 264),     // fabs f11 (X-form xo 264; frc=0 makes A() == X())
        blr(),
    });
    CHECK(Exited(s));
    const auto& c = m.ctx;
    CHECK(c.fpr[4] == 3.75);
    CHECK(c.fpr[5] == 3.375);
    CHECK(c.fpr[6] == -4.375);
    CHECK(c.fpr[7] == 3.75);
    CHECK_EQ(m.memory.Read<u32>(kData + 48), static_cast<u32>(-7));
    CHECK_EQ(m.memory.Read<u64>(kData + 32), std::bit_cast<u64>(3.75));
    CHECK_EQ(m.memory.Read<u32>(kData + 40), std::bit_cast<u32>(3.375f));
    CHECK_EQ((c.cr >> 24) & 0xF, 8);  // cr1 = LT
    CHECK(c.fpr[11] == 7.75);
}

static void TestVmx() {
    std::printf("-- VMX\n");
    using namespace as;
    Machine m;
    for (u32 i = 0; i < 4; ++i) {
        m.memory.Write<u32>(kData + 4 * i, std::bit_cast<u32>(static_cast<f32>(i + 1)));        // 1 2 3 4
        m.memory.Write<u32>(kData + 16 + 4 * i, std::bit_cast<u32>(static_cast<f32>(10 * (i + 1))));  // 10..40
    }
    for (u32 i = 0; i < 32; ++i) m.memory.Write<u8>(kData + 0x40 + i, static_cast<u8>(i));  // bytes 0..31
    const auto s = m.Run({
        lis(9, 2), li(10, 16), li(11, 0x40), li(12, 0x45), li(13, 0x50),
        X(1, 0, 9, 103),                // lvx v1, [0x20000]
        X(2, 10, 9, 103),               // lvx v2, [0x20010]
        VX(3, 1, 2, 10),                // vaddfp v3 = 11 22 33 44
        VX(4, 2, 3, 652),               // vspltw v4 = splat(v3[2]) = 33
        X(3, 10, 9, 231),               // stvx v3, [0x20010]
        X(5, 11, 9, 103),               // lvx v5 = bytes 0..15 (from 0x20040)
        X(6, 13, 9, 103),               // lvx v6 = bytes 16..31
        X(7, 12, 9, 6),                 // lvsl v7 for EA 0x20045
        VA(8, 5, 6, 7, 43),             // vperm v8 = unaligned load from 0x20045: 5..20
        VA(9, 5, 6, 0, 44) | (3u << 6), // vsldoi v9, v5, v6, 3 -> 3..18
        VX(10, 1, 1, 1220),             // vxor v10 = 0
        VX(11, 1, 1, 134 | 0x400),      // vcmpequw. v11 = all ones, CR6 = all true
        blr(),
    });
    CHECK(Exited(s));
    const auto& c = m.ctx;
    auto f = [](const ppu::v128& v, int i) {
        u32 x;
        std::memcpy(&x, v.bytes + 4 * i, 4);
        return std::bit_cast<f32>(ByteSwap(x));
    };
    CHECK(f(c.vr[3], 0) == 11.0f && f(c.vr[3], 3) == 44.0f);
    CHECK(f(c.vr[4], 0) == 33.0f && f(c.vr[4], 3) == 33.0f);
    CHECK_EQ(m.memory.Read<u32>(kData + 16), std::bit_cast<u32>(11.0f));
    bool perm_ok = true, sldoi_ok = true, zero_ok = true, ones_ok = true;
    for (int i = 0; i < 16; ++i) {
        perm_ok &= c.vr[8].bytes[i] == 5 + i;
        sldoi_ok &= c.vr[9].bytes[i] == 3 + i;
        zero_ok &= c.vr[10].bytes[i] == 0;
        ones_ok &= c.vr[11].bytes[i] == 0xFF;
    }
    CHECK(perm_ok);
    CHECK(sldoi_ok);
    CHECK(zero_ok);
    CHECK(ones_ok);
    CHECK_EQ((c.cr >> 4) & 0xF, 8);  // CR6 = all true
}

static void TestHleAndSyscalls() {
    std::printf("-- HLE import call sequence, sc, printf\n");
    using namespace as;
    Machine m;
    // Import slot at 0x20100 holding the OPD of test_add, like a patched libstub entry.
    m.memory.Write<u32>(kData + 0x100, m.add_opd);
    const char hello[] = "hello from PPU\n";
    m.memory.CopyIn(kData + 0x200, hello, sizeof(hello));
    const char fmt[] = "[%d|%s|%x|%5.2f]\n";
    m.memory.CopyIn(kData + 0x300, fmt, sizeof(fmt));
    const char word[] = "MGS4";
    m.memory.CopyIn(kData + 0x340, word, sizeof(word));
    // _sys_printf's OPD via the registry (bound like an import would be).
    m.memory.Write<u32>(kData + 0x104, m.ImportOpd("sysPrxForUser", "_sys_printf"));
    m.memory.Write<u64>(kData + 0x380, std::bit_cast<u64>(3.14159));
    const auto s = m.Run({
        mflr(31),
        lis(9, 2),
        li(3, 40), li(4, 2),
        // PPU import stub: load OPD from slot, save TOC, call through CTR, restore TOC.
        lwz(12, 0x100, 9), lwz(0, 0, 12), std_(2, 40, 1), mtctr(0), lwz(2, 4, 12), bctrl(), ld(2, 40, 1),
        mr(20, 3),                                    // r20 = 42
        // sys_tty_write(0, buf, len, &written)
        li(3, 0), addi(4, 9, 0x200), li(5, sizeof(hello) - 1), addi(6, 9, 0x400), li(11, 403), sc(),
        mr(21, 3),
        // _sys_printf(fmt, -5, "MGS4", 0xBEEF, 3.14159)
        addi(3, 9, 0x300), li(4, -5), addi(5, 9, 0x340), li(6, 0), ori(6, 6, 0xBEEF), ld(7, 0x380, 9),
        lwz(12, 0x104, 9), lwz(0, 0, 12), mtctr(0), bctrl(),
        li(11, 999), sc(), mr(22, 3),                 // unknown syscall -> ENOSYS
        mtlr(31),
        blr(),
    });
    CHECK(Exited(s));
    CHECK_EQ(m.ctx.gpr[20], 42);
    CHECK_EQ(m.ctx.gpr[21], 0);
    CHECK_EQ(m.memory.Read<u32>(kData + 0x400), sizeof(hello) - 1);
    CHECK_EQ(static_cast<s32>(m.ctx.gpr[22]), CELL_ENOSYS);
    CHECK(m.process.tty_output == std::string("hello from PPU\n") + "[-5|MGS4|beef| 3.14]\n");
}

static void TestExitspawn() {
    std::printf("-- sys_game_process_exitspawn2 is reported\n");
    using namespace as;
    Machine m;
    const char path[] = "/dev_bdvd/PS3_GAME/USRDIR/GAME.SELF";
    m.memory.CopyIn(kData + 0x200, path, sizeof(path));
    m.memory.Write<u32>(kData + 0x100, m.ImportOpd("sysPrxForUser", "sys_game_process_exitspawn2"));
    const auto s = m.Run({
        lis(9, 2), addi(3, 9, 0x200), li(4, 0), li(5, 0), li(6, 0), li(7, 0), li(8, 1001), li(9, 0),
        lis(10, 2), lwz(12, 0x100, 10), lwz(0, 0, 12), mtctr(0), bctrl(),
        li(30, 1),  // never reached
        blr(),
    });
    CHECK(s.reason == ppu::StopReason::Exited);
    CHECK(m.ctx.exit_kind == ppu::kExitSpawn);
    CHECK(m.process.exitspawn_path == path);
    CHECK(m.ctx.gpr[30] != 1);
}

static void TestFaultsAndSmc() {
    std::printf("-- precise faults, traps, unimplemented, self-modifying code\n");
    using namespace as;
    {
        Machine m;
        const auto s = m.Run({li(3, 0), lwz(4, 0x10, 3), blr()});  // read from 0x10: null page
        CHECK(s.reason == ppu::StopReason::DataFault);
        CHECK_EQ(s.pc, kCode + 4);
        CHECK_EQ(s.address, 0x10);
        CHECK(!s.write);
        CHECK_EQ(s.executed, 1);
    }
    {
        Machine m;  // text is read-only: storing into it faults as a write
        const auto s = m.Run({lis(3, 1), stw(3, 0, 3), blr()});
        CHECK(s.reason == ppu::StopReason::DataFault && s.write && s.address == kCode);
    }
    {
        Machine m;
        const auto s = m.Run({nop(), 0x7FE00008 /* trap */, blr()});
        CHECK(s.reason == ppu::StopReason::Trap && s.pc == kCode + 4);
    }
    {
        Machine m;
        const auto s = m.Run({nop(), 0x00000000, blr()});
        CHECK(s.reason == ppu::StopReason::Unimplemented && s.pc == kCode + 4);
        CHECK(s.detail.find("primary 0") != std::string::npos);
    }
    {
        Machine m(/*writable_code=*/true);
        // Overwrite the instruction at +0x14 (li r5,1) with li r5,2 before reaching it.
        const u32 patched = as::li(5, 2);
        const auto s = m.Run({
            lis(3, 1),                            // r3 = 0x10000
            lis(4, static_cast<s32>(patched >> 16)), ori(4, 4, patched & 0xFFFF),
            stw(4, 0x14, 3),
            nop(),
            li(5, 1),                             // +0x14: replaced at run time
            blr(),
        });
        CHECK(Exited(s));
        CHECK_EQ(m.ctx.gpr[5], 2);
    }
    {
        Machine m;
        const auto s = m.Run({b(0)}, 1000);  // infinite loop: instruction budget
        CHECK(s.reason == ppu::StopReason::StepLimit && s.executed == 1000);
    }
}


static void TestLbzEdgeCases() {
    std::printf("-- lbz family: RA vs literal 0, update/indexed forms, the MGS4 crash word\n");
    using namespace as;
    {
        Machine m;
        m.memory.Write<u8>(kData + 5, 0x55);
        m.memory.Write<u8>(kData + 6, 0x66);
        m.memory.Write<u8>(kData + 9, 0x99);
        const auto s = m.Run({
            lis(9, 2), addi(9, 9, 5),        // r9 = 0x20005
            0x88090000,                      // lbz r0,0(r9): the exact faulting word, valid base
            mr(20, 0),
            D(35, 4, 9, 1),                  // lbzu r4,1(r9): r4 = [0x20006], r9 = 0x20006
            li(10, 3), X(5, 9, 10, 87),      // lbzx r5,r9,r10 -> [0x20009]
            li(6, -1), lbz(6, 0, 9),         // zero-extends: upper bits cleared
            blr(),
        });
        CHECK(Exited(s));
        CHECK_EQ(m.ctx.gpr[20], 0x55);
        CHECK_EQ(m.ctx.gpr[4], 0x66);
        CHECK_EQ(m.ctx.gpr[9], kData + 6);
        CHECK_EQ(m.ctx.gpr[5], 0x99);
        CHECK_EQ(m.ctx.gpr[6], 0x66);
    }
    {
        // RA field 0 means the literal 0, not r0: with r0 = 0x20000, lbz r3,8(0) reads EA 8.
        Machine m;
        const auto s = m.Run({lis(0, 2), lbz(3, 8, 0), blr()});
        CHECK(s.reason == ppu::StopReason::DataFault);
        CHECK_EQ(s.address, 8);
    }
    {
        // The reported crash: lbz r0,0(r9) with r9 = 0 -> read fault at EA 0, PC on the lbz,
        // nothing committed (r0 keeps its previous value).
        Machine m;
        const auto s = m.Run({li(0, 0x1234), li(9, 0), 0x88090000, blr()});
        CHECK(s.reason == ppu::StopReason::DataFault);
        CHECK_EQ(s.pc, kCode + 8);
        CHECK_EQ(s.address, 0);
        CHECK(!s.write);
        CHECK_EQ(m.ctx.gpr[0], 0x1234);
        const auto ea = ppu::ExplainEffectiveAddress(0x88090000, m.ctx);
        CHECK(ea && ea->find("r9 (0x0)") != std::string::npos && ea->find("= 0x00000000") != std::string::npos);
    }
}

static void TestDisassembler() {
    std::printf("-- disassembler\n");
    auto dis = [](u32 word, u32 pc = 0x10000) { return ppu::Disassemble(pc, word); };
    CHECK(dis(0x88090000) == "lbz r0,0(r9)");
    CHECK(dis(0x7C0802A6) == "mflr r0");
    CHECK(dis(0x7C0803A6) == "mtlr r0");
    CHECK(dis(0x4E800020) == "blr");
    CHECK(dis(0x4E800421) == "bctrl");
    CHECK(dis(0x419E0008) == "beq cr7,0x00010008");
    CHECK(dis(0x4082FFF0) == "bne 0x0000fff0");
    CHECK(dis(0x38600000) == "li r3,0");
    CHECK(dis(0x7C7F1B78) == "mr r31,r3");
    CHECK(dis(0x60000000) == "nop");
    CHECK(dis(0xF821FF81) == "stdu r1,-128(r1)");
    CHECK(dis(0xE8010090) == "ld r0,144(r1)");
    CHECK(dis(0x2F830000) == "cmpwi cr7,r3,0");
    CHECK(dis(0x48000011) == "bl 0x00010010");
    CHECK(dis(0x44000002) == "sc");
    CHECK(dis(0x7FE00008) == "trap");
    CHECK(dis(0x78630020) == "rldicl r3,r3,0,32");
    CHECK(dis(0x00000000) == ".long 0x00000000");
}

int main() {
    TestEncoder();
    TestFnid();
    TestIntegerArithmetic();
    TestRotatesShifts();
    TestBranchesAndCalls();
    TestLoadStore();
    TestAtomics();
    TestFloatingPointClean();
    TestVmx();
    TestHleAndSyscalls();
    TestExitspawn();
    TestFaultsAndSmc();
    TestLbzEdgeCases();
    TestDisassembler();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all PPU tests passed\n");
    return 0;
}
