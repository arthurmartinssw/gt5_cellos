// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/boot_hle_tests.cpp
//
// Phase 7.3: the boot-time HLE that must fill structures instead of returning a bare CELL_OK:
// cellSysutil (video out, audio out, status callbacks, system params, system cache),
// cellGame (boot check, content paths, PARAM.SFO values, game data) and the offline network
// answers (cellNetCtl, sceNp). Layout offsets are checked byte by byte.
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "hle/cell_game.h"
#include "hle/cell_sysutil.h"
#include "hle/fnid.h"
#include "hle/net_offline.h"
#include "ppu_asm.h"
#include "test_machine.h"

using namespace cell;
namespace fs = std::filesystem;

namespace {

constexpr u32 kOut = kData + 0x100;

s32 Call(Machine& m, const char* module, const char* name, std::initializer_list<u64> args) {
    const u32 index = m.hle->Bind(module, hle::ComputeFnid(name));
    const hle::HleFunction& f = m.hle->At(index);
    CHECK(f.implemented);
    ppu::PpuThreadContext c{};
    int r = 3;
    for (u64 a : args) c.gpr[r++] = a;
    f.fn(c, m.memory, f);
    return static_cast<s32>(c.gpr[3]);
}

std::string Str(Machine& m, u32 ea) { return m.memory.ReadCString(ea, 2048).value_or("<bad>"); }

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("cellos_boot_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void TestVideoOut() {
    std::printf("-- cellVideoOut: state, resolution, configure, availability, device info\n");
    for (u16 max : {720, 1080}) {
        Machine m;
        hle::CellSysutil su(*m.kernel, {max});
        su.Register(*m.hle);
        const u8 best = max == 1080 ? 1 : 2;
        m.memory.Fill(kOut, 0xEE, 32);
        CHECK_EQ(Call(m, "cellSysutil", "cellVideoOutGetState", {0, 0, kOut}), CELL_OK);
        CHECK_EQ(m.memory.Read<u8>(kOut + 0), 0);     // ENABLED
        CHECK_EQ(m.memory.Read<u8>(kOut + 1), 1);     // RGB
        CHECK_EQ(m.memory.Read<u8>(kOut + 8), best);  // displayMode.resolutionId
        CHECK_EQ(m.memory.Read<u8>(kOut + 9), 1);     // progressive
        CHECK_EQ(m.memory.Read<u8>(kOut + 11), 2);    // 16:9
        CHECK_EQ(m.memory.Read<u16>(kOut + 14), 1);   // 59.94 Hz
        CHECK_EQ(m.memory.Read<u8>(kOut + 16), 0xEE); // 16 bytes only
        CHECK_EQ(Call(m, "cellSysutil", "cellVideoOutGetState", {1, 0, kOut}), CELL_OK);
        CHECK_EQ(m.memory.Read<u8>(kOut), 1);         // secondary: DISABLED
        CHECK_EQ(Call(m, "cellSysutil", "cellVideoOutGetState", {0, 1, kOut}), hle::CELL_VIDEO_OUT_ERROR_DEVICE_NOT_FOUND);

        CHECK_EQ(Call(m, "cellSysutil", "cellVideoOutGetResolution", {2, kOut}), CELL_OK);
        CHECK_EQ(m.memory.Read<u16>(kOut), 1280);
        CHECK_EQ(m.memory.Read<u16>(kOut + 2), 720);
        CHECK_EQ(Call(m, "cellSysutil", "cellVideoOutGetResolution", {0x0C, kOut}), CELL_OK);
        CHECK_EQ(m.memory.Read<u16>(kOut), 1280);
        CHECK_EQ(m.memory.Read<u16>(kOut + 2), 1080);
        CHECK_EQ(Call(m, "cellSysutil", "cellVideoOutGetResolution", {3, kOut}), hle::CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER);

        CHECK_EQ(Call(m, "cellSysutil", "cellVideoOutGetResolutionAvailability", {0, 2, 0, 0}), 1);
        CHECK_EQ(Call(m, "cellSysutil", "cellVideoOutGetResolutionAvailability", {0, 1, 0, 0}), max == 1080 ? 1 : 0);
        CHECK_EQ(Call(m, "cellSysutil", "cellVideoOutGetResolutionAvailability", {0, 3, 0, 0}), 0);
        CHECK_EQ(Call(m, "cellSysutil", "cellVideoOutGetNumberOfDevice", {0}), 1);

        // configure 720p, read it back
        const u32 cfg = kData + 0x200;
        m.memory.Fill(cfg, 0, 16);
        m.memory.Write<u8>(cfg, 2);
        m.memory.Write<u8>(cfg + 2, 2);
        m.memory.Write<u32>(cfg + 12, 1280 * 4);
        CHECK_EQ(Call(m, "cellSysutil", "cellVideoOutConfigure", {0, cfg, 0, 0}), CELL_OK);
        CHECK_EQ(Call(m, "cellSysutil", "cellVideoOutGetConfiguration", {0, kOut, 0}), CELL_OK);
        CHECK_EQ(m.memory.Read<u8>(kOut), 2);
        CHECK_EQ(m.memory.Read<u32>(kOut + 12), 1280 * 4);
        m.memory.Write<u8>(cfg, 1);
        CHECK_EQ(Call(m, "cellSysutil", "cellVideoOutConfigure", {0, cfg, 0, 0}),
                 max == 1080 ? CELL_OK : hle::CELL_VIDEO_OUT_ERROR_ILLEGAL_CONFIGURATION);

        const u32 info = kData + 0x1000;
        m.memory.Fill(info, 0xEE, 470);
        CHECK_EQ(Call(m, "cellSysutil", "cellVideoOutGetDeviceInfo", {0, 0, info}), CELL_OK);
        CHECK_EQ(m.memory.Read<u8>(info + 0), 1);  // HDMI
        CHECK_EQ(m.memory.Read<u8>(info + 4), max == 1080 ? 8 : 3);
        CHECK_EQ(m.memory.Read<u8>(info + 5), 1);  // available
        CHECK_EQ(m.memory.Read<u8>(info + 32), max == 1080 ? 1 : 2);  // first mode
        CHECK_EQ(m.memory.Read<u8>(info + 456), 0xEE);
    }
}

void TestAudioOut() {
    std::printf("-- cellAudioOut: LPCM stereo 48 kHz\n");
    Machine m;
    hle::CellSysutil su(*m.kernel, {});
    su.Register(*m.hle);
    CHECK_EQ(Call(m, "cellSysutil", "cellAudioOutGetSoundAvailability", {0, 0, 0x04, 0}), 2);
    CHECK_EQ(Call(m, "cellSysutil", "cellAudioOutGetSoundAvailability", {0, 1, 0x04, 0}), 0);  // AC3
    CHECK_EQ(Call(m, "cellSysutil", "cellAudioOutGetSoundAvailability2", {0, 0, 0x04, 2, 0}), 1);
    CHECK_EQ(Call(m, "cellSysutil", "cellAudioOutGetSoundAvailability2", {0, 0, 0x04, 8, 0}), 0);
    m.memory.Fill(kOut, 0xEE, 24);
    CHECK_EQ(Call(m, "cellSysutil", "cellAudioOutGetState", {0, 0, kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u8>(kOut), 0);
    CHECK_EQ(m.memory.Read<u8>(kOut + 13), 2);
    CHECK_EQ(m.memory.Read<u8>(kOut + 14), 0x04);
    CHECK_EQ(m.memory.Read<u32>(kOut + 16), 1);
    CHECK_EQ(m.memory.Read<u8>(kOut + 20), 0xEE);
    CHECK_EQ(Call(m, "cellSysutil", "cellAudioOutGetConfiguration", {0, kOut, 0}), CELL_OK);
    CHECK_EQ(m.memory.Read<u8>(kOut), 2);
    CHECK_EQ(Call(m, "cellSysutil", "cellAudioOutGetNumberOfDevice", {0}), 1);
}

void TestSysutil() {
    std::printf("-- cellSysutil: system params, status callbacks delivered by CheckCallback, system cache\n");
    TempDir tmp;
    Machine m;
    m.kernel->vfs().Mount("/dev_hdd1", tmp.path);
    hle::CellSysutil su(*m.kernel, {});
    su.Register(*m.hle);
    CHECK_EQ(Call(m, "cellSysutil", "cellSysutilGetSystemParamInt", {0x0111, kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut), 1);
    CHECK_EQ(Call(m, "cellSysutil", "cellSysutilGetSystemParamInt", {0x0112, kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut), 1);
    CHECK_EQ(Call(m, "cellSysutil", "cellSysutilGetSystemParamInt", {0x9999, kOut}), hle::CELL_SYSUTIL_ERROR_VALUE);
    CHECK_EQ(Call(m, "cellSysutil", "cellSysutilGetSystemParamString", {0x0113, kData + 0x400, 128}), CELL_OK);
    CHECK(Str(m, kData + 0x400) == "Player");
    CHECK_EQ(Call(m, "cellSysutil", "cellSysutilGetSystemParamString", {0x0113, kData + 0x400, 16}), hle::CELL_SYSUTIL_ERROR_SIZE);

    // callback: stores (status, param, userdata) at kData+0x700
    using namespace as;
    const u32 code = kCode + 0x900;
    m.Load({lis(9, 2), stw(3, 0x700, 9), stw(4, 0x704, 9), stw(5, 0x708, 9), blr()}, code);
    m.memory.Write<u32>(kData + 0x600, code);
    m.memory.Write<u32>(kData + 0x604, 0);
    CHECK_EQ(Call(m, "cellSysutil", "cellSysutilRegisterCallback", {0, kData + 0x600, 0x55}), CELL_OK);
    CHECK_EQ(Call(m, "cellSysutil", "cellSysutilRegisterCallback", {4, kData + 0x600, 0}), hle::CELL_SYSUTIL_ERROR_VALUE);
    CHECK_EQ(Call(m, "cellSysutil", "cellSysutilCheckCallback", {}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kData + 0x700), 0);  // nothing pending: not called
    su.PostEvent(hle::CELL_SYSUTIL_REQUEST_EXITGAME, 7);
    CHECK_EQ(Call(m, "cellSysutil", "cellSysutilCheckCallback", {}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kData + 0x700), hle::CELL_SYSUTIL_REQUEST_EXITGAME);
    CHECK_EQ(m.memory.Read<u32>(kData + 0x704), 7);
    CHECK_EQ(m.memory.Read<u32>(kData + 0x708), 0x55);

    // system cache
    const u32 param = kData + 0x2000;
    m.memory.Fill(param, 0, 1100);
    m.memory.CopyIn(param, "GT5CACHE", 9);
    CHECK_EQ(Call(m, "cellSysutil", "cellSysCacheMount", {param}), hle::CELL_SYSCACHE_RET_OK_CLEARED);
    CHECK(Str(m, param + 32) == "/dev_hdd1/caches/GT5CACHE");
    CHECK(fs::is_directory(tmp.path / "caches" / "GT5CACHE"));
    CHECK_EQ(Call(m, "cellSysutil", "cellSysCacheMount", {param}), hle::CELL_SYSCACHE_RET_OK_RELAYED);
    { std::ofstream(tmp.path / "caches" / "GT5CACHE" / "x.bin") << "x"; }
    CHECK_EQ(Call(m, "cellSysutil", "cellSysCacheClear", {}), hle::CELL_SYSCACHE_RET_OK_CLEARED);
    CHECK(fs::is_empty(tmp.path / "caches" / "GT5CACHE"));
    m.memory.Fill(param, 0, 32);
    CHECK_EQ(Call(m, "cellSysutil", "cellSysCacheMount", {param}), hle::CELL_SYSCACHE_ERROR_PARAM);
}

void TestCellGame() {
    std::printf("-- cellGame: boot check, content permit, PARAM.SFO values, game data, patch\n");
    TempDir tmp;
    for (bool disc : {true, false}) {
        Machine m;
        m.kernel->vfs().Mount("/dev_hdd0", tmp.path);
        m.process.disc_boot = disc;
        m.process.title_id = "BCUS98114";
        m.process.sfo_text = {{"TITLE", "Gran Turismo 5"}, {"TITLE_ID", "BCUS98114"}, {"APP_VER", "01.00"}, {"TITLE_01", "GT5 (en)"}};
        m.process.sfo_int = {{"RESOLUTION", 0x3F}, {"PARENTAL_LEVEL", 3}};
        hle::CellGame game(*m.kernel);
        game.Register(*m.hle);
        const u32 size = kData + 0x300, dir = kData + 0x320;
        m.memory.Fill(dir, 0xEE, 32);
        CHECK_EQ(Call(m, "cellGame", "cellGameBootCheck", {kOut, kOut + 4, size, dir}), CELL_OK);
        CHECK_EQ(m.memory.Read<u32>(kOut), disc ? hle::CELL_GAME_GAMETYPE_DISC : hle::CELL_GAME_GAMETYPE_HDD);
        CHECK_EQ(m.memory.Read<u32>(kOut + 4), 0);
        CHECK_EQ(m.memory.Read<u32>(size + 4), 0xFFFFFFFFu);  // sizeKB not computed
        CHECK(m.memory.Read<u32>(size) > 0);                  // free space of the hdd0 folder
        CHECK(Str(m, dir) == (disc ? "" : "BCUS98114"));
        CHECK_EQ(Call(m, "cellGame", "cellGameContentPermit", {kData + 0x400, kData + 0x480}), CELL_OK);
        CHECK(Str(m, kData + 0x400) == (disc ? "/dev_bdvd/PS3_GAME" : "/dev_hdd0/game/BCUS98114"));
        CHECK(Str(m, kData + 0x480) == (disc ? "/dev_bdvd/PS3_GAME/USRDIR" : "/dev_hdd0/game/BCUS98114/USRDIR"));

        CHECK_EQ(Call(m, "cellGame", "cellGameGetParamString", {100, kData + 0x500, 16}), CELL_OK);
        CHECK(Str(m, kData + 0x500) == "BCUS98114");
        CHECK_EQ(Call(m, "cellGame", "cellGameGetParamString", {0, kData + 0x500, 128}), CELL_OK);
        CHECK(Str(m, kData + 0x500) == "Gran Turismo 5");
        CHECK_EQ(Call(m, "cellGame", "cellGameGetParamString", {3, kData + 0x500, 128}), CELL_OK);  // TITLE_01
        CHECK(Str(m, kData + 0x500) == "GT5 (en)");
        CHECK_EQ(Call(m, "cellGame", "cellGameGetParamString", {0, kData + 0x500, 5}), CELL_OK);  // truncated
        CHECK(Str(m, kData + 0x500) == "Gran");
        CHECK_EQ(Call(m, "cellGame", "cellGameGetParamString", {999, kData + 0x500, 16}), hle::CELL_GAME_ERROR_PARAM);
        CHECK_EQ(Call(m, "cellGame", "cellGameGetParamInt", {103, kOut}), CELL_OK);
        CHECK_EQ(m.memory.Read<u32>(kOut), 0x3F);
        CHECK_EQ(Call(m, "cellGame", "cellGameGetParamInt", {102, kOut}), CELL_OK);
        CHECK_EQ(m.memory.Read<u32>(kOut), 3);
        CHECK_EQ(Call(m, "cellGame", "cellGameGetParamInt", {999, kOut}), hle::CELL_GAME_ERROR_PARAM);

        m.memory.CopyIn(kData + 0x600, "BCUS98114DATA", 14);
        fs::remove_all(tmp.path / "game");
        CHECK_EQ(Call(m, "cellGame", "cellGameDataCheck", {hle::CELL_GAME_GAMETYPE_GAMEDATA, kData + 0x600, size}), hle::CELL_GAME_RET_NONE);
        fs::create_directories(tmp.path / "game" / "BCUS98114DATA");
        CHECK_EQ(Call(m, "cellGame", "cellGameDataCheck", {hle::CELL_GAME_GAMETYPE_GAMEDATA, kData + 0x600, size}), hle::CELL_GAME_RET_OK);
        CHECK_EQ(Call(m, "cellGame", "cellGamePatchCheck", {size, 0}), hle::CELL_GAME_ERROR_NOTPATCH);
    }
}

void TestNetOffline() {
    std::printf("-- network offline: cellNetCtl disconnected, sceNp offline\n");
    Machine m;
    hle::NetOffline net;
    net.Register(*m.hle);
    m.memory.Write<u32>(kOut, 0xEEEEEEEE);
    CHECK_EQ(Call(m, "cellNetCtl", "cellNetCtlInit", {}), CELL_OK);
    CHECK_EQ(Call(m, "cellNetCtl", "cellNetCtlGetState", {kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut), hle::CELL_NET_CTL_STATE_Disconnected);
    CHECK_EQ(Call(m, "cellNetCtl", "cellNetCtlGetInfo", {1, kOut}), hle::CELL_NET_CTL_ERROR_NOT_CONNECTED);
    CHECK_EQ(Call(m, "cellNetCtl", "cellNetCtlAddHandler", {0x1234, 0, kOut}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOut), 1);
    CHECK_EQ(Call(m, "cellNetCtl", "cellNetCtlGetState", {0}), CELL_EFAULT);
    CHECK_EQ(Call(m, "sceNp", "sceNpInit", {0x10000, kData + 0x1000}), CELL_OK);
    CHECK_EQ(Call(m, "sceNp", "sceNpManagerGetStatus", {kOut}), CELL_OK);
    CHECK_EQ(static_cast<s32>(m.memory.Read<u32>(kOut)), hle::SCE_NP_MANAGER_STATUS_OFFLINE);
    CHECK_EQ(Call(m, "sys_net", "sys_net_initialize_network_ex", {kData + 0x1000}), CELL_OK);
}

}  // namespace

int main() {
    log::Threshold() = log::Level::Error;
    TestVideoOut();
    TestAudioOut();
    TestSysutil();
    TestCellGame();
    TestNetOffline();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all boot HLE tests passed\n");
    return 0;
}
