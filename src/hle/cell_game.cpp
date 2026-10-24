// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_game.cpp
#include "hle/cell_game.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "common/log.h"

namespace cell::hle {

namespace {

using C = ppu::PpuThreadContext;
using M = vm::GuestMemory;

u32 A32(const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); }
void Return(C& c, s32 v) { c.gpr[3] = static_cast<u64>(static_cast<s64>(v)); }
lv2::Kernel& K(const HleFunction& f) { return static_cast<CellGame*>(f.user)->kernel(); }

void PutString(M& m, u32 ea, u32 capacity, const std::string& s) {
    m.Fill(ea, 0, capacity);
    m.CopyIn(ea, s.data(), std::min<std::size_t>(s.size(), capacity - 1));
}

// CellGameContentSize: be32 hddFreeSizeKB, be32 sizeKB, be32 sysSizeKB
void WriteSize(lv2::Kernel& k, M& m, u32 ea) {
    if (!ea || !m.Check(ea, 12, vm::kPageWritable)) return;
    s64 free_kb = 0;
    if (const auto hdd = k.vfs().Resolve("/dev_hdd0")) {
        std::error_code ec;
        const auto space = std::filesystem::space(*hdd, ec);
        if (!ec) free_kb = static_cast<s64>(space.available / 1024);
    }
    m.Write<u32>(ea + 0, static_cast<u32>(std::min<s64>(free_kb, 0x7FFFFFFF)));
    m.Write<u32>(ea + 4, static_cast<u32>(-1));  // CELL_GAME_SIZEKB_NOTCALC
    m.Write<u32>(ea + 8, 0);
}

std::string GameDir(lv2::Kernel& k) {
    return k.process().disc_boot ? "/dev_bdvd/PS3_GAME" : "/dev_hdd0/game/" + k.process().title_id;
}

// cellGameBootCheck(u32* type, u32* attributes, CellGameContentSize* size, char dirName[32])
void cellGameBootCheck(C& c, M& m, const HleFunction& f) {
    lv2::Kernel& k = K(f);
    const u32 type = A32(c, 0), attributes = A32(c, 1), size = A32(c, 2), dir = A32(c, 3);
    if (!type || !m.Check(type, 4, vm::kPageWritable) || !attributes || !m.Check(attributes, 4, vm::kPageWritable))
        return Return(c, CELL_GAME_ERROR_PARAM);
    const bool disc = k.process().disc_boot;
    m.Write<u32>(type, disc ? CELL_GAME_GAMETYPE_DISC : CELL_GAME_GAMETYPE_HDD);
    m.Write<u32>(attributes, 0);
    WriteSize(k, m, size);
    if (dir && m.Check(dir, 32, vm::kPageWritable)) PutString(m, dir, 32, disc ? std::string() : k.process().title_id);
    LOG_INFO("cellGame", "cellGameBootCheck: %s game %s", disc ? "disc" : "HDD", k.process().title_id.c_str());
    Return(c, CELL_OK);
}

// cellGameContentPermit(char contentInfoPath[128], char usrdirPath[128])
void cellGameContentPermit(C& c, M& m, const HleFunction& f) {
    lv2::Kernel& k = K(f);
    const u32 info = A32(c, 0), usrdir = A32(c, 1);
    if (!info || !usrdir || !m.Check(info, 128, vm::kPageWritable) || !m.Check(usrdir, 128, vm::kPageWritable))
        return Return(c, CELL_GAME_ERROR_PARAM);
    const std::string dir = GameDir(k);
    PutString(m, info, 128, dir);
    PutString(m, usrdir, 128, dir + "/USRDIR");
    LOG_INFO("cellGame", "cellGameContentPermit: %s/USRDIR", dir.c_str());
    Return(c, CELL_OK);
}

// cellGameDataCheck(u32 type, const char* dirName, CellGameContentSize* size)
void cellGameDataCheck(C& c, M& m, const HleFunction& f) {
    lv2::Kernel& k = K(f);
    const u32 type = A32(c, 0), size = A32(c, 2);
    std::string name;
    if (A32(c, 1)) name = m.ReadCString(A32(c, 1), 32).value_or("");
    if (type == CELL_GAME_GAMETYPE_DISC) {
        WriteSize(k, m, size);
        return Return(c, k.process().disc_boot ? CELL_GAME_RET_OK : CELL_GAME_RET_NONE);
    }
    if (name.empty()) return Return(c, CELL_GAME_ERROR_PARAM);
    WriteSize(k, m, size);
    const std::string guest = "/dev_hdd0/game/" + name;
    const auto host = k.vfs().Resolve(guest);
    std::error_code ec;
    const bool present = host && std::filesystem::is_directory(*host, ec);
    LOG_INFO("cellGame", "cellGameDataCheck(type %u, \"%s\"): %s", type, name.c_str(), present ? "present" : "none");
    Return(c, present ? CELL_GAME_RET_OK : CELL_GAME_RET_NONE);
}

// cellGameGetParamInt(s32 id, s32* value)
void cellGameGetParamInt(C& c, M& m, const HleFunction& f) {
    lv2::Kernel& k = K(f);
    const u32 id = A32(c, 0), out = A32(c, 1);
    if (!out || !m.Check(out, 4, vm::kPageWritable)) return Return(c, CELL_GAME_ERROR_PARAM);
    const char* key = id == 102 ? "PARENTAL_LEVEL" : id == 103 ? "RESOLUTION" : id == 104 ? "SOUND_FORMAT" : nullptr;
    if (!key) return Return(c, CELL_GAME_ERROR_PARAM);
    const auto& ints = k.process().sfo_int;
    const auto it = ints.find(key);
    m.Write<u32>(out, it == ints.end() ? 0 : it->second);
    Return(c, CELL_OK);
}

// cellGameGetParamString(s32 id, char* buf, u32 bufsize)
void cellGameGetParamString(C& c, M& m, const HleFunction& f) {
    lv2::Kernel& k = K(f);
    const u32 id = A32(c, 0), buf = A32(c, 1), size = A32(c, 2);
    if (!buf || !size || !m.Check(buf, size, vm::kPageWritable)) return Return(c, CELL_GAME_ERROR_PARAM);
    std::string key;
    if (id == 0 || id == 1) key = "TITLE";  // current language / default
    else if (id >= 2 && id <= 21) {
        char k2[16];
        std::snprintf(k2, sizeof(k2), "TITLE_%02u", id - 2);
        key = k2;
    } else if (id == 100) key = "TITLE_ID";
    else if (id == 101) key = "VERSION";
    else if (id == 105) key = "PS3_SYSTEM_VER";
    else if (id == 106) key = "APP_VER";
    else return Return(c, CELL_GAME_ERROR_PARAM);
    const auto& text = k.process().sfo_text;
    auto it = text.find(key);
    if (it == text.end() && id <= 21) it = text.find("TITLE");
    PutString(m, buf, size, it == text.end() ? std::string() : it->second);
    Return(c, CELL_OK);
}

void cellGamePatchCheck(C& c, M&, const HleFunction&) { Return(c, CELL_GAME_ERROR_NOTPATCH); }

// cellGameContentErrorDialog(s32 type, s32 errNeedSizeKB, const char* dirName)
void cellGameContentErrorDialog(C& c, M&, const HleFunction&) {
    LOG_WARN("cellGame", "cellGameContentErrorDialog(type %d, need %d KB): the game reports a content error",
             static_cast<s32>(A32(c, 0)), static_cast<s32>(A32(c, 1)));
    Return(c, CELL_OK);
}

}  // namespace

void CellGame::Register(HleRegistry& r) {
    struct Entry {
        const char* name;
        HleFunc fn;
    };
    static constexpr Entry kEntries[] = {
        {"cellGameBootCheck", cellGameBootCheck},
        {"cellGameContentPermit", cellGameContentPermit},
        {"cellGameDataCheck", cellGameDataCheck},
        {"cellGameGetParamInt", cellGameGetParamInt},
        {"cellGameGetParamString", cellGameGetParamString},
        {"cellGamePatchCheck", cellGamePatchCheck},
        {"cellGameContentErrorDialog", cellGameContentErrorDialog},
    };
    for (const Entry& e : kEntries) r.Register("cellGame", e.name, e.fn, this);
}

}  // namespace cell::hle
