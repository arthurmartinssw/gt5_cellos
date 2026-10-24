// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_sysutil.h
//
// cellSysutil: the system-utility exports every game calls before its first frame. A logging
// stub that returns CELL_OK is dangerous here, because these functions fill structures the game
// then trusts (the video mode it sizes its render targets from, the language, the cache path).
// What is implemented, with the values a retail console on an HDMI TV would report:
//
//   video out  cellVideoOutGetState / GetResolution / Configure / GetConfiguration /
//              GetResolutionAvailability / GetNumberOfDevice / GetDeviceInfo
//              Output: HDMI, RGB, 59.94 Hz progressive, 16:9; the highest mode is 720p by
//              default or 1080p (--resolution 1080). Resolution ids 1 1080, 2 720, 4 480,
//              5 576, 0xA..0xD the 1080-line horizontal-scaled modes (1600/1440/1280/960).
//   audio out  cellAudioOutGetState / GetSoundAvailability(2) / Configure / GetConfiguration /
//              GetNumberOfDevice / GetDeviceInfo / SetCopyControl: LPCM 2 channels at 48 kHz on
//              HDMI (no audio backend yet).
//   callbacks  cellSysutilRegisterCallback / UnregisterCallback / CheckCallback: four slots;
//              PostEvent() queues a status (e.g. CELL_SYSUTIL_REQUEST_EXITGAME) that the next
//              CheckCallback delivers to every registered callback (status, param, userdata),
//              synchronously on a helper PPU thread.
//   params     cellSysutilGetSystemParamInt / GetSystemParamString (language English (US),
//              cross = enter, 24-hour clock, no parental lock, nickname "Player").
//   cache      cellSysCacheMount / cellSysCacheClear: /dev_hdd1/caches/<cacheId>.
// Struct layouts and constants follow the SDK headers as reproduced by RPCS3 (cellVideoOut.h,
// cellAudioOut.h, cellSysutil.h, cellSysCache.h).
#pragma once

#include <deque>
#include <mutex>
#include <utility>

#include "hle/hle_registry.h"
#include "lv2/kernel.h"

namespace cell::hle {

inline constexpr u32 CELL_SYSUTIL_REQUEST_EXITGAME = 0x0101;
inline constexpr u32 CELL_SYSUTIL_DRAWING_BEGIN = 0x0121;
inline constexpr u32 CELL_SYSUTIL_DRAWING_END = 0x0122;
inline constexpr u32 CELL_SYSUTIL_SYSTEM_MENU_OPEN = 0x0131;
inline constexpr u32 CELL_SYSUTIL_SYSTEM_MENU_CLOSE = 0x0132;

inline constexpr s32 CELL_SYSUTIL_ERROR_TYPE = static_cast<s32>(0x8002b101u);
inline constexpr s32 CELL_SYSUTIL_ERROR_VALUE = static_cast<s32>(0x8002b102u);
inline constexpr s32 CELL_SYSUTIL_ERROR_SIZE = static_cast<s32>(0x8002b103u);
inline constexpr s32 CELL_VIDEO_OUT_ERROR_ILLEGAL_CONFIGURATION = static_cast<s32>(0x8002b221u);
inline constexpr s32 CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER = static_cast<s32>(0x8002b222u);
inline constexpr s32 CELL_VIDEO_OUT_ERROR_DEVICE_NOT_FOUND = static_cast<s32>(0x8002b224u);
inline constexpr s32 CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT = static_cast<s32>(0x8002b225u);
inline constexpr s32 CELL_AUDIO_OUT_ERROR_ILLEGAL_CONFIGURATION = static_cast<s32>(0x8002b241u);
inline constexpr s32 CELL_AUDIO_OUT_ERROR_DEVICE_NOT_FOUND = static_cast<s32>(0x8002b244u);
inline constexpr s32 CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT = static_cast<s32>(0x8002b245u);
inline constexpr s32 CELL_SYSCACHE_RET_OK_CLEARED = 0;
inline constexpr s32 CELL_SYSCACHE_RET_OK_RELAYED = 1;
inline constexpr s32 CELL_SYSCACHE_ERROR_ACCESS_ERROR = static_cast<s32>(0x8002bc01u);
inline constexpr s32 CELL_SYSCACHE_ERROR_PARAM = static_cast<s32>(0x8002bc03u);
inline constexpr s32 CELL_SYSCACHE_ERROR_NOTMOUNTED = static_cast<s32>(0x8002bc04u);

struct VideoMode {
    u8 id;
    u16 width;
    u16 height;
};
// Resolution id -> size; nullptr for unknown ids.
[[nodiscard]] const VideoMode* FindVideoMode(u8 id) noexcept;

class CellSysutil {
public:
    struct Config {
        u16 max_height = 720;  // 720 or 1080: the best mode the "TV" accepts
    };

    CellSysutil(lv2::Kernel& kernel, Config config) noexcept;
    void Register(HleRegistry& registry);

    // Queues a sysutil status for the next cellSysutilCheckCallback.
    void PostEvent(u32 status, u64 param = 0);

    lv2::Kernel& kernel() noexcept { return kernel_; }
    [[nodiscard]] const Config& config() const noexcept { return config_; }

    // ---- state (guarded by mutex) ----
    std::mutex mutex;
    struct Callback {
        u32 func = 0;  // OPD, 0 = empty slot
        u32 userdata = 0;
    };
    Callback callbacks[4];
    std::deque<std::pair<u32, u64>> pending;  // status, param
    u8 video_resolution = 0;  // configured mode (0: not configured yet -> best mode)
    u8 video_format = 0;
    u8 video_aspect = 2;
    u32 video_pitch = 0;
    std::string cache_path;  // guest path mounted by cellSysCacheMount

private:
    lv2::Kernel& kernel_;
    Config config_;
};

}  // namespace cell::hle
