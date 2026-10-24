// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_game.h
//
// cellGame: how a game learns where it lives. A disc title calls cellGameBootCheck, then
// cellGameContentPermit, and from then on opens files under the returned USRDIR path; it may
// also look for its installed data / updates with cellGameDataCheck and read PARAM.SFO values.
//   cellGameBootCheck(u32* type, u32* attributes, CellGameContentSize* size, char dirName[32])
//       type DISC (1) when booted from /dev_bdvd, HDD (2) otherwise (dirName = title id).
//       CellGameContentSize: be32 hddFreeSizeKB, be32 sizeKB (-1: not computed), be32 sysSizeKB.
//   cellGameContentPermit(char contentInfoPath[128], char usrdirPath[128])
//       disc: /dev_bdvd/PS3_GAME and /dev_bdvd/PS3_GAME/USRDIR; HDD: /dev_hdd0/game/<id>/...
//   cellGameDataCheck(u32 type, const char* dirName, CellGameContentSize* size)
//       /dev_hdd0/game/<dirName> present: CELL_GAME_RET_OK, else CELL_GAME_RET_NONE.
//   cellGameGetParamInt / cellGameGetParamString: PARAM.SFO of the running title.
//   cellGamePatchCheck: a disc boot is not a patch -> CELL_GAME_ERROR_NOTPATCH.
//   cellGameContentErrorDialog: no dialog to show; logged, CELL_OK.
// Constants per the SDK as reproduced in RPCS3's cellGame.h.
#pragma once

#include "hle/hle_registry.h"
#include "lv2/kernel.h"

namespace cell::hle {

inline constexpr s32 CELL_GAME_RET_OK = 0;
inline constexpr s32 CELL_GAME_RET_CANCEL = 1;
inline constexpr s32 CELL_GAME_RET_NONE = 2;
inline constexpr s32 CELL_GAME_ERROR_NOTFOUND = static_cast<s32>(0x8002cb04u);
inline constexpr s32 CELL_GAME_ERROR_PARAM = static_cast<s32>(0x8002cb07u);
inline constexpr s32 CELL_GAME_ERROR_NOTPATCH = static_cast<s32>(0x8002cb27u);
inline constexpr u32 CELL_GAME_GAMETYPE_DISC = 1;
inline constexpr u32 CELL_GAME_GAMETYPE_HDD = 2;
inline constexpr u32 CELL_GAME_GAMETYPE_GAMEDATA = 3;

class CellGame {
public:
    explicit CellGame(lv2::Kernel& kernel) noexcept : kernel_(kernel) {}
    void Register(HleRegistry& registry);
    lv2::Kernel& kernel() noexcept { return kernel_; }

private:
    lv2::Kernel& kernel_;
};

}  // namespace cell::hle
