// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — spu/spu_decoder.cpp
#include "spu/spu_decoder.h"

#include <array>
#include <cstdio>

#include "spu/spu_defs.h"

namespace cell::spu {

namespace {

constexpr SpuOpInfo kInfo[] = {
    {"<invalid>", SpuForm::RR, 0, 11},
#define SPU_X_INFO(name, mnem, form, opcode, bits) {mnem, SpuForm::form, opcode, bits},
    SPU_INSTRUCTION_LIST(SPU_X_INFO)
#undef SPU_X_INFO
};
static_assert(std::size(kInfo) == static_cast<std::size_t>(SpuOp::Count));

struct Table {
    std::array<SpuOp, 2048> ops{};
    std::string conflicts;
    Table() {
        for (std::size_t i = 1; i < std::size(kInfo); ++i) {
            const SpuOpInfo& info = kInfo[i];
            const u32 span = 1u << (11 - info.bits);
            const u32 first = static_cast<u32>(info.opcode) << (11 - info.bits);
            for (u32 slot = first; slot < first + span; ++slot) {
                if (ops[slot] != SpuOp::Invalid) {
                    conflicts += info.mnemonic;
                    conflicts += " vs ";
                    conflicts += kInfo[static_cast<std::size_t>(ops[slot])].mnemonic;
                    conflicts += "; ";
                }
                ops[slot] = static_cast<SpuOp>(i);
            }
        }
    }
};

const Table& GetTable() {
    static const Table table;
    return table;
}

std::string Reg(u32 r) {
    char t[8];
    std::snprintf(t, sizeof(t), "$%u", r);
    return t;
}

}  // namespace

SpuOp Decode(u32 instruction) noexcept { return GetTable().ops[instruction >> 21]; }
const SpuOpInfo& Info(SpuOp op) noexcept { return kInfo[static_cast<std::size_t>(op)]; }
std::string DecoderConflicts() { return GetTable().conflicts; }

std::string Disassemble(u32 pc, u32 i) {
    const SpuOp op = Decode(i);
    const SpuOpInfo& info = Info(op);
    char text[96];
    if (op == SpuOp::Invalid) {
        std::snprintf(text, sizeof(text), ".long 0x%08x", i);
        return text;
    }
    const std::string rt = Reg(FieldRT(i)), ra = Reg(FieldRA(i)), rb = Reg(FieldRB(i));
    const char* m = info.mnemonic;
    switch (op) {
    case SpuOp::STOP:
    case SpuOp::STOPD: std::snprintf(text, sizeof(text), "%s 0x%x", m, i & 0x3FFF); return text;
    case SpuOp::LNOP:
    case SpuOp::NOP:
    case SpuOp::SYNC:
    case SpuOp::DSYNC:
    case SpuOp::IRET: return m;
    case SpuOp::RDCH:
    case SpuOp::RCHCNT: std::snprintf(text, sizeof(text), "%s %s,$ch%u", m, rt.c_str(), FieldRA(i)); return text;
    case SpuOp::WRCH: std::snprintf(text, sizeof(text), "%s $ch%u,%s", m, FieldRA(i), rt.c_str()); return text;
    case SpuOp::BI:
    case SpuOp::HBR: std::snprintf(text, sizeof(text), "%s %s", m, ra.c_str()); return text;
    case SpuOp::LQD:
    case SpuOp::STQD:
        std::snprintf(text, sizeof(text), "%s %s,%d(%s)", m, rt.c_str(), FieldI10(i) * 16, ra.c_str());
        return text;
    case SpuOp::BR:
    case SpuOp::BRSL:
    case SpuOp::BRZ:
    case SpuOp::BRNZ:
    case SpuOp::BRHZ:
    case SpuOp::BRHNZ:
    case SpuOp::LQR:
    case SpuOp::STQR: {
        const u32 target = (pc + (static_cast<u32>(FieldS16(i)) << 2)) & kLsMask;
        if (op == SpuOp::BR) std::snprintf(text, sizeof(text), "%s 0x%05x", m, target);
        else std::snprintf(text, sizeof(text), "%s %s,0x%05x", m, rt.c_str(), target);
        return text;
    }
    case SpuOp::BRA:
    case SpuOp::BRASL:
    case SpuOp::LQA:
    case SpuOp::STQA: {
        const u32 target = (static_cast<u32>(FieldS16(i)) << 2) & kLsMask;
        if (op == SpuOp::BRA) std::snprintf(text, sizeof(text), "%s 0x%05x", m, target);
        else std::snprintf(text, sizeof(text), "%s %s,0x%05x", m, rt.c_str(), target);
        return text;
    }
    default: break;
    }
    switch (info.form) {
    case SpuForm::RR: std::snprintf(text, sizeof(text), "%s %s,%s,%s", m, rt.c_str(), ra.c_str(), rb.c_str()); break;
    case SpuForm::RRR:
        std::snprintf(text, sizeof(text), "%s $%u,%s,%s,%s", m, FieldRT4(i), ra.c_str(), rb.c_str(), rt.c_str());
        break;
    case SpuForm::RI7: std::snprintf(text, sizeof(text), "%s %s,%s,%d", m, rt.c_str(), ra.c_str(), FieldI7(i)); break;
    case SpuForm::RI8: std::snprintf(text, sizeof(text), "%s %s,%s,%u", m, rt.c_str(), ra.c_str(), FieldI8(i)); break;
    case SpuForm::RI10: std::snprintf(text, sizeof(text), "%s %s,%s,%d", m, rt.c_str(), ra.c_str(), FieldI10(i)); break;
    case SpuForm::RI16: std::snprintf(text, sizeof(text), "%s %s,0x%x", m, rt.c_str(), FieldI16(i)); break;
    case SpuForm::RI18: std::snprintf(text, sizeof(text), "%s %s,0x%x", m, rt.c_str(), FieldI18(i)); break;
    }
    return text;
}

}  // namespace cell::spu
