// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/spu_isa_tests.cpp
//
// Phase 6: SPU execution engine. Every expected value below is derived by hand from the SPU
// ISA definitions, not from the interpreter, and real encodings are checked against known
// SPU machine code so the opcode table cannot drift.
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#include "hle/fnid.h"
#include "hle/sys_spu_hle.h"
#include "lv2/sys_event.h"
#include "lv2/sys_spu.h"
#include "spu/spu_decoder.h"
#include "spu/spu_interpreter.h"
#include "spu_asm.h"
#include "test_machine.h"

using namespace cell;
using spu::SpuOp;

namespace {

struct Q {
    u32 w[4];
};
u32 Fb(float f) { return std::bit_cast<u32>(f); }
u64 Db(double d) { return std::bit_cast<u64>(d); }

// One SPU thread with a real LS in the arena, run synchronously on the test thread.
struct Rig {
    Machine m;
    std::shared_ptr<spu::SpuThreadGroup> group = std::make_shared<spu::SpuThreadGroup>();
    std::shared_ptr<spu::SpuThread> t;
    u64 executed = 0;

    Rig() {
        group->kernel = m.kernel.get();
        group->num = 1;
        group->threads.resize(1);
        group->state = spu::SpuThreadGroup::State::Running;
        group->running = 1;
        t = std::make_shared<spu::SpuThread>(*m.kernel, *group, 0);
        const auto ea = m.memory.Allocate(vm::Area::SpuThread, spu::kLsWindow, spu::kLsWindow, vm::kProtReadWrite);
        t->ctx.ls_ea = *ea;
        t->ctx.ls = m.memory.shadow() + *ea;
        group->threads[0] = t;
    }
    void Load(const std::vector<u32>& words, u32 at = 0) {
        for (std::size_t k = 0; k < words.size(); ++k) {
            const u32 be = ByteSwap(words[k]);
            std::memcpy(t->ctx.ls + at + 4 * k, &be, 4);
        }
    }
    void Set(u32 r, Q q) {
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 4; ++j) t->ctx.gpr[r].bytes[4 * k + j] = static_cast<u8>(q.w[k] >> (24 - 8 * j));
    }
    Q Get(u32 r) const {
        Q q{};
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 4; ++j) q.w[k] = q.w[k] << 8 | t->ctx.gpr[r].bytes[4 * k + j];
        return q;
    }
    spu::SpuExit Run(u32 pc = 0) {
        {
            std::lock_guard lock(t->m);
            t->exit = spu::SpuExit::None;
            t->out_mbox.reset();
            t->in_mbox.clear();
        }
        t->ctx.pc = pc;
        spu::SpuInterpreter cpu;
        cpu.Run(*t);
        executed += cpu.executed();
        std::lock_guard lock(t->m);
        return t->exit;
    }
};

bool Same(const Q& a, const Q& b) { return std::memcmp(a.w, b.w, 16) == 0; }

// ---------------------------------------------------------------------------------------

void TestEncodings() {
    std::printf("-- SPU opcode table: no conflicts, known encodings, disassembly\n");
    CHECK(spu::DecoderConflicts().empty());
    if (!spu::DecoderConflicts().empty()) std::fprintf(stderr, "  %s\n", spu::DecoderConflicts().c_str());
    CHECK_EQ(sa::Ai(1, 1, -32), 0x1CF80081);       // ai $1,$1,-32 (function prologue)
    CHECK_EQ(sa::Stqd(0, 16, 1), 0x24004080);      // stqd $0,16($1)
    CHECK_EQ(sa::Lqd(0, 16, 1), 0x34004080);       // lqd $0,16($1)
    CHECK_EQ(sa::RR(SpuOp::NOP, 0, 0, 0), 0x40200000);
    CHECK_EQ(sa::RR(SpuOp::LNOP, 0, 0, 0), 0x00200000);
    CHECK_EQ(sa::Bi(0), 0x35000000);               // bi $0 (return)
    CHECK_EQ(sa::Il(3, 0), 0x40800003);            // il $3,0
    CHECK(spu::Decode(0x1CF80081) == SpuOp::AI);
    CHECK(spu::Decode(0x24004080) == SpuOp::STQD);
    CHECK(spu::Decode(0x35000000) == SpuOp::BI);
    CHECK(spu::Decode(0xB0000000) == SpuOp::SHUFB);
    CHECK(spu::Decode(0x00800000) == SpuOp::Invalid);
    CHECK(spu::Disassemble(0, 0x1CF80081) == "ai $1,$1,-32");
    CHECK(spu::Disassemble(0, 0x24004080) == "stqd $0,16($1)");
    CHECK(spu::Disassemble(0x100, sa::Brsl(0, 8)) == "brsl $0,0x00120");
    CHECK(spu::Disassemble(0, sa::Wrch(28, 3)) == "wrch $ch28,$3");
    CHECK(spu::Disassemble(0, sa::Stop(0x102)) == "stop 0x102");
}

struct Case {
    const char* name;
    u32 insn;  // reads $1 (a), $2 (b), $3 (c); writes $4
    Q a, b, c, expect;
};

void TestInstructions() {
    std::printf("-- SPU instruction semantics (integer, logical, shifts, masks, float, double)\n");
    using namespace sa;
    const Q z{{0, 0, 0, 0}};
    const Q bytes0{{0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F}};
    const Q bytes1{{0x10111213, 0x14151617, 0x18191A1B, 0x1C1D1E1F}};
    const Case cases[] = {
        {"a", RR(SpuOp::A, 4, 1, 2), {{1, 2, 3, 0xFFFFFFFF}}, {{10, 20, 30, 1}}, z, {{11, 22, 33, 0}}},
        {"sf", RR(SpuOp::SF, 4, 1, 2), {{1, 2, 3, 4}}, {{10, 20, 30, 40}}, z, {{9, 18, 27, 36}}},
        {"ai", RI10(SpuOp::AI, 4, 1, -32), {{100, 0, 32, 1}}, z, z, {{68, 0xFFFFFFE0, 0, 0xFFFFFFE1}}},
        {"sfi", RI10(SpuOp::SFI, 4, 1, 10), {{3, 10, 11, 0}}, z, z, {{7, 0, 0xFFFFFFFF, 10}}},
        {"ah", RR(SpuOp::AH, 4, 1, 2), {{0x0001FFFF, 0, 0, 0}}, {{0x00010001, 0, 0, 0}}, z, {{0x00020000, 0, 0, 0}}},
        {"cg", RR(SpuOp::CG, 4, 1, 2), {{0xFFFFFFFF, 1, 0x80000000, 0}}, {{1, 1, 0x80000000, 0}}, z, {{1, 0, 1, 0}}},
        {"bg", RR(SpuOp::BG, 4, 1, 2), {{1, 5, 0, 7}}, {{2, 4, 0, 7}}, z, {{1, 0, 1, 1}}},
        {"mpy", RR(SpuOp::MPY, 4, 1, 2), {{0x0000FFFF, 3, 0x12340002, 0x8000}}, {{2, 0xFFFFFFFD, 3, 2}}, z,
         {{0xFFFFFFFE, 0xFFFFFFF7, 6, 0xFFFF0000}}},
        {"mpyu", RR(SpuOp::MPYU, 4, 1, 2), {{0xFFFF, 2, 0, 0}}, {{0xFFFF, 3, 0, 0}}, z, {{0xFFFE0001, 6, 0, 0}}},
        {"mpyh", RR(SpuOp::MPYH, 4, 1, 2), {{0x00020000, 0, 0, 0}}, {{3, 0, 0, 0}}, z, {{0x00060000, 0, 0, 0}}},
        {"mpys", RR(SpuOp::MPYS, 4, 1, 2), {{0x4000, 0xFFFF, 0, 0}}, {{0x4000, 1, 0, 0}}, z, {{0x1000, 0xFFFFFFFF, 0, 0}}},
        {"mpyhhu", RR(SpuOp::MPYHHU, 4, 1, 2), {{0xFFFF0000, 0, 0, 0}}, {{0x00020000, 0, 0, 0}}, z, {{0x1FFFE, 0, 0, 0}}},
        {"mpyi", RI10(SpuOp::MPYI, 4, 1, -3), {{5, 0x10000 | 2, 0, 0}}, z, z, {{0xFFFFFFF1, 0xFFFFFFFA, 0, 0}}},
        {"mpya", RRR(SpuOp::MPYA, 4, 1, 2, 3), {{3, 0, 0, 0}}, {{4, 0, 0, 0}}, {{100, 1, 2, 3}}, {{112, 1, 2, 3}}},
        {"clz", RR(SpuOp::CLZ, 4, 1, 0), {{0, 1, 0x80000000, 0x00010000}}, z, z, {{32, 31, 0, 15}}},
        {"cntb", RR(SpuOp::CNTB, 4, 1, 0), {{0xFF010300, 0, 0, 0}}, z, z, {{0x08010200, 0, 0, 0}}},
        {"sumb", RR(SpuOp::SUMB, 4, 1, 2), {{0x01020304, 0, 0, 0}}, {{0x10101010, 0, 0, 0}}, z, {{0x0040000A, 0, 0, 0}}},
        {"avgb", RR(SpuOp::AVGB, 4, 1, 2), {{0x01FF0000, 0, 0, 0}}, {{0x02FF0001, 0, 0, 0}}, z, {{0x02FF0001, 0, 0, 0}}},
        {"absdb", RR(SpuOp::ABSDB, 4, 1, 2), {{0x0510FF00, 0, 0, 0}}, {{0x10050000, 0, 0, 0}}, z, {{0x0B0BFF00, 0, 0, 0}}},
        {"xsbh", RR(SpuOp::XSBH, 4, 1, 0), {{0x00800001, 0, 0, 0}}, z, z, {{0xFF800001, 0, 0, 0}}},
        {"xshw", RR(SpuOp::XSHW, 4, 1, 0), {{0x12348000, 0x7FFF, 0, 0}}, z, z, {{0xFFFF8000, 0x7FFF, 0, 0}}},
        {"xswd", RR(SpuOp::XSWD, 4, 1, 0), {{7, 0x80000000, 9, 5}}, z, z, {{0xFFFFFFFF, 0x80000000, 0, 5}}},
        {"and", RR(SpuOp::AND, 4, 1, 2), {{0xFF00FF00, 0, 0, 0}}, {{0x0FF00FF0, 0, 0, 0}}, z, {{0x0F000F00, 0, 0, 0}}},
        {"andc", RR(SpuOp::ANDC, 4, 1, 2), {{0xFF00FF00, 0, 0, 0}}, {{0x0FF00FF0, 0, 0, 0}}, z, {{0xF000F000, 0, 0, 0}}},
        {"orc", RR(SpuOp::ORC, 4, 1, 2), {{0, 0, 0, 0}}, {{0xFFFF0000, 0xFFFFFFFF, 0, 0}}, z, {{0x0000FFFF, 0, 0xFFFFFFFF, 0xFFFFFFFF}}},
        {"eqv", RR(SpuOp::EQV, 4, 1, 2), {{0xF0F0F0F0, 0, 0, 0}}, {{0xFF00FF00, 0, 0, 0}}, z, {{0xF00FF00F, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF}}},
        {"andbi", RI10(SpuOp::ANDBI, 4, 1, 0xF0), {{0xFFFFFFFF, 0x0F0F0F0F, 0, 0}}, z, z, {{0xF0F0F0F0, 0, 0, 0}}},
        {"orhi", RI10(SpuOp::ORHI, 4, 1, -2), {{0x00010000, 0, 0, 0}}, z, z, {{0xFFFFFFFE, 0xFFFEFFFE, 0xFFFEFFFE, 0xFFFEFFFE}}},
        {"xori", RI10(SpuOp::XORI, 4, 1, 0x1FF), {{0xFF, 0, 0, 0}}, z, z, {{0x100, 0x1FF, 0x1FF, 0x1FF}}},
        {"orx", RR(SpuOp::ORX, 4, 1, 0), {{1, 2, 4, 8}}, z, z, {{15, 0, 0, 0}}},
        {"selb", RRR(SpuOp::SELB, 4, 1, 2, 3), {{0x12345678, 0, 0, 0}}, {{0x9ABCDEF0, 0, 0, 0}}, {{0xFFFF0000, 0, 0, 0}},
         {{0x9ABC5678, 0, 0, 0}}},
        {"shufb", RRR(SpuOp::SHUFB, 4, 1, 2, 3), bytes0, bytes1, {{0x1F0080C0, 0xE0101112, 0x03020100, 0x0F0E0D0C}},
         {{0x1F0000FF, 0x80101112, 0x03020100, 0x0F0E0D0C}}},
        {"ceq", RR(SpuOp::CEQ, 4, 1, 2), {{1, 2, 3, 4}}, {{1, 0, 3, 0}}, z, {{0xFFFFFFFF, 0, 0xFFFFFFFF, 0}}},
        {"cgt", RR(SpuOp::CGT, 4, 1, 2), {{0xFFFFFFFF, 2, 0, 0}}, {{1, 1, 0, 0}}, z, {{0, 0xFFFFFFFF, 0, 0}}},
        {"clgt", RR(SpuOp::CLGT, 4, 1, 2), {{0xFFFFFFFF, 2, 0, 0}}, {{1, 1, 0, 0}}, z, {{0xFFFFFFFF, 0xFFFFFFFF, 0, 0}}},
        {"cgtbi", RI10(SpuOp::CGTBI, 4, 1, -1), {{0x00FF7F80, 0, 0, 0}}, z, z, {{0xFF00FF00, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF}}},
        {"ceqhi", RI10(SpuOp::CEQHI, 4, 1, -1), {{0xFFFF0001, 0, 0, 0}}, z, z, {{0xFFFF0000, 0, 0, 0}}},
        {"clgti", RI10(SpuOp::CLGTI, 4, 1, 10), {{11, 10, 0xFFFFFFFF, 0}}, z, z, {{0xFFFFFFFF, 0, 0xFFFFFFFF, 0}}},
        {"shl", RR(SpuOp::SHL, 4, 1, 2), {{1, 1, 3, 0}}, {{31, 32, 4, 0}}, z, {{0x80000000, 0, 48, 0}}},
        {"shlhi", RI7(SpuOp::SHLHI, 4, 1, 4), {{0x00010F00, 0, 0, 0}}, z, z, {{0x0010F000, 0, 0, 0}}},
        {"rot", RR(SpuOp::ROT, 4, 1, 2), {{0x80000001, 0x12345678, 0, 0}}, {{1, 8, 0, 0}}, z, {{3, 0x34567812, 0, 0}}},
        {"roth", RR(SpuOp::ROTH, 4, 1, 2), {{0x80010001, 0, 0, 0}}, {{0x00010004, 0, 0, 0}}, z, {{0x00030010, 0, 0, 0}}},
        {"rotm", RR(SpuOp::ROTM, 4, 1, 2), {{0x80000000, 0x80000000, 0, 0}}, {{0xFFFFFFFC, 0xFFFFFFE0, 0, 0}}, z, {{0x08000000, 0, 0, 0}}},
        {"rotma", RR(SpuOp::ROTMA, 4, 1, 2), {{0x80000000, 0x80000000, 0x40, 0}}, {{0xFFFFFFFC, 0xFFFFFFE0, 0xFFFFFFFF, 0}}, z,
         {{0xF8000000, 0xFFFFFFFF, 0x20, 0}}},
        {"rotmi", RI7(SpuOp::ROTMI, 4, 1, -4), {{0xF0, 0, 0, 0}}, z, z, {{0xF, 0, 0, 0}}},
        {"rotmahi", RI7(SpuOp::ROTMAHI, 4, 1, -1), {{0x80000002, 0, 0, 0}}, z, z, {{0xC0000001, 0, 0, 0}}},
        {"rotqbyi", RI7(SpuOp::ROTQBYI, 4, 1, 4), {{1, 2, 3, 4}}, z, z, {{2, 3, 4, 1}}},
        {"shlqbyi", RI7(SpuOp::SHLQBYI, 4, 1, 4), {{1, 2, 3, 4}}, z, z, {{2, 3, 4, 0}}},
        {"rotqmbyi", RI7(SpuOp::ROTQMBYI, 4, 1, -4), {{1, 2, 3, 4}}, z, z, {{0, 1, 2, 3}}},
        {"rotqby", RR(SpuOp::ROTQBY, 4, 1, 2), {{1, 2, 3, 4}}, {{8, 0, 0, 0}}, z, {{3, 4, 1, 2}}},
        {"shlqbybi", RR(SpuOp::SHLQBYBI, 4, 1, 2), {{1, 2, 3, 4}}, {{32, 0, 0, 0}}, z, {{2, 3, 4, 0}}},
        {"rotqmbybi", RR(SpuOp::ROTQMBYBI, 4, 1, 2), {{1, 2, 3, 4}}, {{0xFFFFFFE0, 0, 0, 0}}, z, {{0, 1, 2, 3}}},
        {"shlqbii", RI7(SpuOp::SHLQBII, 4, 1, 1), {{0x80000000, 0x80000000, 0, 1}}, z, z, {{1, 0, 0, 2}}},
        {"rotqbii", RI7(SpuOp::ROTQBII, 4, 1, 1), {{0x80000000, 0, 0, 1}}, z, z, {{0, 0, 0, 3}}},
        {"rotqmbii", RI7(SpuOp::ROTQMBII, 4, 1, -1), {{1, 0, 0, 2}}, z, z, {{0, 0x80000000, 0, 1}}},
        {"fsmbi", RI16(SpuOp::FSMBI, 4, 0xF00F), z, z, z, {{0xFFFFFFFF, 0, 0, 0xFFFFFFFF}}},
        {"fsmh", RR(SpuOp::FSMH, 4, 1, 0), {{0x81, 0, 0, 0}}, z, z, {{0xFFFF0000, 0, 0, 0x0000FFFF}}},
        {"fsm", RR(SpuOp::FSM, 4, 1, 0), {{0x5, 0, 0, 0}}, z, z, {{0, 0xFFFFFFFF, 0, 0xFFFFFFFF}}},
        {"gbb", RR(SpuOp::GBB, 4, 1, 0), {{0x01000101, 0, 0, 1}}, z, z, {{0xB001, 0, 0, 0}}},
        {"gb", RR(SpuOp::GB, 4, 1, 0), {{1, 0, 3, 2}}, z, z, {{0xA, 0, 0, 0}}},
        {"cwd", RI7(SpuOp::CWD, 4, 1, 0), {{0x1004, 0, 0, 0}}, z, z, {{0x10111213, 0x00010203, 0x18191A1B, 0x1C1D1E1F}}},
        {"cbd", RI7(SpuOp::CBD, 4, 1, 3), {{0, 0, 0, 0}}, z, z, {{0x10111203, 0x14151617, 0x18191A1B, 0x1C1D1E1F}}},
        {"cdx", RR(SpuOp::CDX, 4, 1, 2), {{4, 0, 0, 0}}, {{4, 0, 0, 0}}, z, {{0x10111213, 0x14151617, 0x00010203, 0x04050607}}},
        {"il", RI16(SpuOp::IL, 4, -1), z, z, z, {{0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF}}},
        {"ilh", RI16(SpuOp::ILH, 4, 0xABCD), z, z, z, {{0xABCDABCD, 0xABCDABCD, 0xABCDABCD, 0xABCDABCD}}},
        {"ila", RI18(SpuOp::ILA, 4, 0x3FFFF), z, z, z, {{0x3FFFF, 0x3FFFF, 0x3FFFF, 0x3FFFF}}},
        {"fa", RR(SpuOp::FA, 4, 1, 2), {{Fb(1.5f), 0, 0, 0}}, {{Fb(2.25f), 0, 0, 0}}, z, {{Fb(3.75f), 0, 0, 0}}},
        {"fs", RR(SpuOp::FS, 4, 1, 2), {{Fb(1.5f), 0, 0, 0}}, {{Fb(2.25f), 0, 0, 0}}, z, {{Fb(-0.75f), 0, 0, 0}}},
        {"fm", RR(SpuOp::FM, 4, 1, 2), {{Fb(2.0f), 0, 0, 0}}, {{Fb(3.0f), 0, 0, 0}}, z, {{Fb(6.0f), 0, 0, 0}}},
        {"fma", RRR(SpuOp::FMA, 4, 1, 2, 3), {{Fb(2.0f), 0, 0, 0}}, {{Fb(3.0f), 0, 0, 0}}, {{Fb(1.0f), 0, 0, 0}}, {{Fb(7.0f), 0, 0, 0}}},
        {"fnms", RRR(SpuOp::FNMS, 4, 1, 2, 3), {{Fb(2.0f), 0, 0, 0}}, {{Fb(3.0f), 0, 0, 0}}, {{Fb(1.0f), 0, 0, 0}}, {{Fb(-5.0f), 0, 0, 0}}},
        {"fms", RRR(SpuOp::FMS, 4, 1, 2, 3), {{Fb(2.0f), 0, 0, 0}}, {{Fb(3.0f), 0, 0, 0}}, {{Fb(1.0f), 0, 0, 0}}, {{Fb(5.0f), 0, 0, 0}}},
        {"fcgt", RR(SpuOp::FCGT, 4, 1, 2), {{Fb(2.0f), Fb(-3.0f), 0, 0}}, {{Fb(1.0f), Fb(1.0f), 0, 0}}, z, {{0xFFFFFFFF, 0, 0, 0}}},
        {"fcmgt", RR(SpuOp::FCMGT, 4, 1, 2), {{Fb(2.0f), Fb(-3.0f), 0, 0}}, {{Fb(1.0f), Fb(1.0f), 0, 0}}, z, {{0xFFFFFFFF, 0xFFFFFFFF, 0, 0}}},
        {"fm overflow saturates", RR(SpuOp::FM, 4, 1, 2), {{Fb(3e38f), 0, 0, 0}}, {{Fb(10.0f), 0, 0, 0}}, z, {{0x7F7FFFFF, 0, 0, 0}}},
        {"fa denormal flush", RR(SpuOp::FA, 4, 1, 2), {{0x00000001, 0, 0, 0}}, {{0, 0, 0, 0}}, z, {{0, 0, 0, 0}}},
        {"cflts", RI8(SpuOp::CFLTS, 4, 1, 173), {{Fb(3.75f), Fb(-3.75f), Fb(3e9f), Fb(-3e9f)}}, z, z, {{3, 0xFFFFFFFD, 0x7FFFFFFF, 0x80000000}}},
        {"cflts scale", RI8(SpuOp::CFLTS, 4, 1, 172), {{Fb(1.5f), 0, 0, 0}}, z, z, {{3, 0, 0, 0}}},
        {"cfltu", RI8(SpuOp::CFLTU, 4, 1, 173), {{Fb(-1.0f), Fb(5e9f), Fb(7.9f), 0}}, z, z, {{0, 0xFFFFFFFF, 7, 0}}},
        {"csflt", RI8(SpuOp::CSFLT, 4, 1, 155), {{0xFFFFFFFB, 0, 0, 0}}, z, z, {{Fb(-5.0f), 0, 0, 0}}},
        {"csflt scale", RI8(SpuOp::CSFLT, 4, 1, 154), {{3, 0, 0, 0}}, z, z, {{Fb(1.5f), 0, 0, 0}}},
        {"cuflt", RI8(SpuOp::CUFLT, 4, 1, 155), {{0x80000000, 0, 0, 0}}, z, z, {{Fb(2147483648.0f), 0, 0, 0}}},
        {"frest", RR(SpuOp::FREST, 4, 1, 0), {{Fb(4.0f), 0, 0, 0}}, z, z, {{Fb(0.25f), 0x7F7FFFFF, 0x7F7FFFFF, 0x7F7FFFFF}}},
        {"fi passes the estimate", RR(SpuOp::FI, 4, 1, 2), {{1, 2, 3, 4}}, {{5, 6, 7, 8}}, z, {{5, 6, 7, 8}}},
        {"dfa", RR(SpuOp::DFA, 4, 1, 2), {{u32(Db(1.5) >> 32), u32(Db(1.5)), u32(Db(-1.0) >> 32), u32(Db(-1.0))}},
         {{u32(Db(2.25) >> 32), u32(Db(2.25)), u32(Db(0.5) >> 32), u32(Db(0.5))}}, z,
         {{u32(Db(3.75) >> 32), u32(Db(3.75)), u32(Db(-0.5) >> 32), u32(Db(-0.5))}}},
        {"fesd", RR(SpuOp::FESD, 4, 1, 0), {{Fb(1.5f), 0, Fb(-2.0f), 0}}, z, z,
         {{u32(Db(1.5) >> 32), u32(Db(1.5)), u32(Db(-2.0) >> 32), u32(Db(-2.0))}}},
        {"frds", RR(SpuOp::FRDS, 4, 1, 0), {{u32(Db(1.5) >> 32), u32(Db(1.5)), u32(Db(-2.0) >> 32), u32(Db(-2.0))}}, z, z,
         {{Fb(1.5f), 0, Fb(-2.0f), 0}}},
    };
    for (const Case& k : cases) {
        Rig rig;
        rig.Set(1, k.a);
        rig.Set(2, k.b);
        rig.Set(3, k.c);
        rig.Set(127, {{0, 0, 0, 0}});
        std::vector<u32> program{k.insn};
        for (u32 w : sa::Exit(127)) program.push_back(w);
        rig.Load(program);
        const auto exit = rig.Run();
        const Q got = rig.Get(4);
        if (exit != spu::SpuExit::ThreadExit || !Same(got, k.expect)) {
            ++g_failures;
            std::fprintf(stderr, "FAIL %s: got %08x %08x %08x %08x, want %08x %08x %08x %08x\n", k.name, got.w[0], got.w[1],
                         got.w[2], got.w[3], k.expect.w[0], k.expect.w[1], k.expect.w[2], k.expect.w[3]);
        }
    }
}

void TestCarryChains() {
    std::printf("-- SPU extended arithmetic: 64-bit add through cg/addx, sfx and bgx with carry-in\n");
    using namespace sa;
    // 0x00000001_FFFFFFFF + 0x00000000_00000001 = 0x00000002_00000000 (doubleword 0)
    Rig rig;
    rig.Set(1, {{1, 0xFFFFFFFF, 0, 0}});
    rig.Set(2, {{0, 1, 0, 0}});
    rig.Set(127, {{0, 0, 0, 0}});
    std::vector<u32> p{RR(SpuOp::CG, 5, 1, 2), RI7(SpuOp::SHLQBYI, 5, 5, 4), RR(SpuOp::ADDX, 5, 1, 2)};
    for (u32 w : Exit(127)) p.push_back(w);
    rig.Load(p);
    CHECK(rig.Run() == spu::SpuExit::ThreadExit);
    CHECK_EQ(rig.Get(5).w[0], 2);
    CHECK_EQ(rig.Get(5).w[1], 0);

    // sfx: rt = rb - ra - (1 - (rt & 1));  bgx: (rt & 1) ? rb >= ra : rb > ra
    Rig r2;
    r2.Set(1, {{3, 3, 6, 0}});
    r2.Set(2, {{10, 10, 5, 0}});
    r2.Set(4, {{1, 0, 1, 1}});
    r2.Set(5, {{1, 0, 0, 1}});
    r2.Set(127, {{0, 0, 0, 0}});
    std::vector<u32> q{RR(SpuOp::SFX, 4, 1, 2), RR(SpuOp::BGX, 5, 1, 2)};
    for (u32 w : Exit(127)) q.push_back(w);
    r2.Load(q);
    CHECK(r2.Run() == spu::SpuExit::ThreadExit);
    CHECK(Same(r2.Get(4), {{7, 6, 0xFFFFFFFF, 0}}));
    CHECK(Same(r2.Get(5), {{1, 1, 0, 1}}));
}

void TestControlFlowAndMemory() {
    std::printf("-- SPU branches, calls, loads/stores\n");
    using namespace sa;
    Rig rig;
    rig.Set(127, {{0, 0, 0, 0}});
    // sum 1..10 in a loop, call a function adding 100, then add 400; exit with the sum
    rig.Load({
        Il(5, 0),                        // 0x00
        Il(6, 10),                       // 0x04
        RR(SpuOp::A, 5, 5, 6),           // 0x08 loop
        Ai(6, 6, -1),                    // 0x0c
        Brnz(6, -2),                     // 0x10 -> 0x08
        Brsl(0, 3),                      // 0x14 -> 0x20
        Ai(5, 5, 400),                   // 0x18 (I10 is -512..511)
        Br(3),                           // 0x1c -> 0x28
        Ai(5, 5, 100),                   // 0x20 function
        Bi(0),                           // 0x24 return
        Wrch(28, 5),                     // 0x28
        Stop(0x102),
    });
    CHECK(rig.Run() == spu::SpuExit::ThreadExit);
    CHECK_EQ(rig.t->exit_status, 55 + 100 + 400);
    CHECK_EQ(rig.Get(0).w[0], 0x18);  // link register from brsl

    Rig mem;
    mem.Set(1, {{1, 2, 3, 4}});
    mem.Set(2, {{0x1000, 0, 0, 0}});
    mem.Set(7, {{0x10, 0, 0, 0}});
    mem.Set(127, {{0, 0, 0, 0}});
    mem.Load({
        Stqd(1, 16, 2),                         // LS[0x1010] = $1
        Lqd(3, 16, 2),                          // $3 = LS[0x1010]
        RR(SpuOp::LQX, 4, 2, 7),                // $4 = LS[0x1000 + 0x10]
        RI16(SpuOp::LQA, 8, 0x1010 >> 2),       // $8 = LS[0x1010]
        RI16(SpuOp::STQR, 1, (0x2000 - 0x10) >> 2),  // LS[pc + 0x1ff0] = LS[0x2000] (pc = 0x10)
        Lqd(9, 0x1000, 2),                      // $9 = LS[0x2000]
        Wrch(28, 127),
        Stop(0x102),
    });
    CHECK(mem.Run() == spu::SpuExit::ThreadExit);
    CHECK(Same(mem.Get(3), {{1, 2, 3, 4}}));
    CHECK(Same(mem.Get(4), {{1, 2, 3, 4}}));
    CHECK(Same(mem.Get(8), {{1, 2, 3, 4}}));
    CHECK(Same(mem.Get(9), {{1, 2, 3, 4}}));
    u32 word;
    std::memcpy(&word, mem.t->ctx.ls + 0x101C, 4);
    CHECK_EQ(ByteSwap(word), 4);  // big-endian in the LS
}

void TestChannelsAndDma() {
    std::printf("-- SPU program doing MFC DMA through channels (GET, add, PUT) and exiting with a status\n");
    using namespace sa;
    Rig rig;
    for (u32 k = 0; k < 4; ++k) rig.m.memory.Write<u32>(kData + 4 * k, 10 + k);
    std::vector<u32> p{Il(10, 0x2000), Wrch(16, 10), Il(11, 0), Wrch(17, 11)};
    for (u32 w : Load32(12, kData)) p.push_back(w);
    p.insert(p.end(), {Wrch(18, 12), Il(13, 16), Wrch(19, 13), Il(14, 0), Wrch(20, 14), Il(15, 0x40), Wrch(21, 15)});  // GET
    p.insert(p.end(), {Lqd(16, 0, 10), RR(SpuOp::A, 16, 16, 16), Stqd(16, 0, 10)});
    p.insert(p.end(), {Ai(12, 12, 16), Wrch(18, 12), Il(15, 0x20), Wrch(21, 15)});  // PUT to kData+16
    p.insert(p.end(), {Il(17, 1), Wrch(22, 17), Il(18, 2), Wrch(23, 18), Rdch(19, 24)});  // tag status
    p.insert(p.end(), {Wrch(28, 16), Stop(0x102)});
    rig.Load(p);
    CHECK(rig.Run() == spu::SpuExit::ThreadExit);
    for (u32 k = 0; k < 4; ++k) CHECK_EQ(rig.m.memory.Read<u32>(kData + 16 + 4 * k), 2 * (10 + k));
    CHECK_EQ(rig.t->exit_status, 20);
    CHECK_EQ(rig.Get(19).w[0], 1);
}

void TestFaultReport() {
    std::printf("-- SPU invalid instruction ends the process with a disassembled report\n");
    Rig rig;
    rig.Load({sa::Il(3, 1), 0x00800000});
    CHECK(rig.Run() == spu::SpuExit::Fault);
    CHECK(rig.m.kernel->is_terminating());
    const auto end = rig.m.kernel->process_end();
    CHECK(end.message.find("unknown SPU instruction: 0x00800000") != std::string::npos);
    CHECK(end.message.find("LS pc 0x00004") != std::string::npos);
}

// The real path: SPU ELF in guest memory -> sys_spu_image_import -> thread group syscalls ->
// host thread running the interpreter -> sys_spu_thread_exit -> join from the PPU side.
void TestThreadGroupEndToEnd() {
    std::printf("-- end to end: SPU ELF + lv2 thread group run on the interpreter, joined by the PPU\n");
    Machine m;
    hle::SysSpuHle spu_hle(*m.kernel);
    spu_hle.Register(*m.hle);
    m.kernel->spu().SetExecutorFactory(spu::MakeSpuInterpreter);
    // SPU program at LS 0x100: $3 holds argument 1 in its preferred doubleword (high word
    // = 7): exit(7 + 35).
    std::vector<u32> code{sa::Ai(3, 3, 35), sa::Wrch(28, 3), sa::Stop(0x102)};
    const u32 elf = kData + 0x400, payload = 0x80;
    m.memory.Fill(elf, 0, 0x100);
    const u8 ident[] = {0x7F, 'E', 'L', 'F', 1, 2, 1};
    m.memory.CopyIn(elf, ident, sizeof(ident));
    m.memory.Write<u16>(elf + 16, 2);
    m.memory.Write<u16>(elf + 18, 23);
    m.memory.Write<u32>(elf + 24, 0x100);
    m.memory.Write<u32>(elf + 28, 52);
    m.memory.Write<u16>(elf + 42, 32);
    m.memory.Write<u16>(elf + 44, 1);
    m.memory.Write<u32>(elf + 52 + 0, 1);
    m.memory.Write<u32>(elf + 52 + 4, payload);
    m.memory.Write<u32>(elf + 52 + 8, 0x100);
    m.memory.Write<u32>(elf + 52 + 16, static_cast<u32>(code.size() * 4));
    m.memory.Write<u32>(elf + 52 + 20, static_cast<u32>(code.size() * 4));
    for (std::size_t k = 0; k < code.size(); ++k) m.memory.Write<u32>(elf + payload + 4 * static_cast<u32>(k), code[k]);

    auto sys = [&](u32 n, std::initializer_list<u64> args) {
        ppu::PpuThreadContext c{};
        c.gpr[11] = n;
        int r = 3;
        for (u64 a : args) c.gpr[r++] = a;
        m.syscalls.Dispatch(c);
        return static_cast<s32>(c.gpr[3]);
    };
    const u32 img = kData + 0x180, gattr = kData + 0x000, tattr = kData + 0x080, args = kData + 0x100, out = kData + 0x200;
    {
        const u32 index = m.hle->Bind("sysPrxForUser", hle::ComputeFnid("sys_spu_image_import"));
        const auto& f = m.hle->At(index);
        ppu::PpuThreadContext c{};
        c.gpr[3] = img;
        c.gpr[4] = elf;
        c.gpr[5] = spu::SYS_SPU_IMAGE_PROTECT;
        f.fn(c, m.memory, f);
        CHECK_EQ(static_cast<s32>(c.gpr[3]), CELL_OK);
    }
    m.memory.Fill(gattr, 0, 16);
    m.memory.Fill(tattr, 0, 12);
    m.memory.Fill(args, 0, 32);
    m.memory.Write<u64>(args, u64{7} << 32);
    CHECK_EQ(sys(170, {out, 1, 100, gattr}), CELL_OK);
    const u32 group = m.memory.Read<u32>(out);
    CHECK_EQ(sys(172, {out + 4, group, 0, img, tattr, args}), CELL_OK);
    const u32 thread = m.memory.Read<u32>(out + 4);
    CHECK_EQ(sys(173, {group}), CELL_OK);
    CHECK_EQ(sys(178, {group, out + 8, out + 12}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(out + 8), spu::SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXIT);
    CHECK_EQ(sys(165, {thread, out + 16}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(out + 16), 42);
    CHECK(!m.kernel->is_terminating());
}

void TestThroughput() {
    std::printf("-- interpreter throughput (informative)\n");
    using namespace sa;
    Rig rig;
    rig.Set(127, {{0, 0, 0, 0}});
    // 2M iterations of a 3-instruction loop
    std::vector<u32> p;
    for (u32 w : Load32(6, 2'000'000)) p.push_back(w);
    p.insert(p.end(), {Il(5, 0), RR(SpuOp::A, 5, 5, 6), Ai(6, 6, -1), Brnz(6, -2), Wrch(28, 127), Stop(0x102)});
    rig.Load(p);
    const auto start = std::chrono::steady_clock::now();
    CHECK(rig.Run() == spu::SpuExit::ThreadExit);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("   %.0f M SPU instructions/s\n", static_cast<double>(rig.executed) / s / 1e6);
}

}  // namespace

int main() {
    log::Threshold() = log::Level::Error;
    TestEncodings();
    TestInstructions();
    TestCarryChains();
    TestControlFlowAndMemory();
    TestChannelsAndDma();
    TestFaultReport();
    TestThreadGroupEndToEnd();
    TestThroughput();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all SPU ISA tests passed\n");
    return 0;
}
