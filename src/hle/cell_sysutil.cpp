// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_sysutil.cpp
#include "hle/cell_sysutil.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "common/log.h"
#include "lv2/sys_ppu_thread.h"

namespace cell::hle {

namespace {

using C = ppu::PpuThreadContext;
using M = vm::GuestMemory;

u32 A32(const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); }
void Return(C& c, s32 v) { c.gpr[3] = static_cast<u64>(static_cast<s64>(v)); }
CellSysutil& Self(const HleFunction& f) { return *static_cast<CellSysutil*>(f.user); }
bool Writable(const M& m, u32 ea, u32 size) { return ea && m.Check(ea, size, vm::kPageWritable); }

constexpr VideoMode kModes[] = {
    {0x01, 1920, 1080}, {0x02, 1280, 720}, {0x04, 720, 480},   {0x05, 720, 576},
    {0x0A, 1600, 1080}, {0x0B, 1440, 1080}, {0x0C, 1280, 1080}, {0x0D, 960, 1080},
};

constexpr u8 CELL_VIDEO_OUT_PRIMARY = 0;
constexpr u8 CELL_VIDEO_OUT_OUTPUT_STATE_ENABLED = 0;
constexpr u8 CELL_VIDEO_OUT_OUTPUT_STATE_DISABLED = 1;
constexpr u8 CELL_VIDEO_OUT_COLOR_SPACE_RGB = 0x01;
constexpr u8 CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE = 1;
constexpr u8 CELL_VIDEO_OUT_ASPECT_16_9 = 2;
constexpr u16 CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ = 0x0001;
constexpr u8 CELL_VIDEO_OUT_PORT_HDMI = 0x01;
constexpr u8 CELL_VIDEO_OUT_DEVICE_STATE_AVAILABLE = 1;

bool Available(const CellSysutil& s, const VideoMode& v) { return v.height <= s.config().max_height; }

u8 BestMode(const CellSysutil& s) { return s.config().max_height >= 1080 ? 0x01 : 0x02; }

u8 CurrentMode(CellSysutil& s) { return s.video_resolution ? s.video_resolution : BestMode(s); }

// CellVideoOutDisplayMode (8 bytes): u8 resolutionId, scanMode, conversion, aspect; u8 reserved[2]; be16 refreshRates
void WriteDisplayMode(M& m, u32 ea, u8 resolution) {
    m.Fill(ea, 0, 8);
    m.Write<u8>(ea + 0, resolution);
    m.Write<u8>(ea + 1, CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE);
    m.Write<u8>(ea + 2, 0);  // CELL_VIDEO_OUT_DISPLAY_CONVERSION_NONE
    m.Write<u8>(ea + 3, CELL_VIDEO_OUT_ASPECT_16_9);
    m.Write<u16>(ea + 6, CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ);
}

// ---------------------------------------------------------------------------------------
// Video out
// ---------------------------------------------------------------------------------------

// cellVideoOutGetState(u32 videoOut, u32 deviceIndex, CellVideoOutState* state)
// CellVideoOutState (16 bytes): u8 state, u8 colorSpace, u8 reserved[6], CellVideoOutDisplayMode displayMode
void cellVideoOutGetState(C& c, M& m, const HleFunction& f) {
    CellSysutil& s = Self(f);
    const u32 out = A32(c, 0), device = A32(c, 1), state = A32(c, 2);
    if (!Writable(m, state, 16)) return Return(c, CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER);
    if (out > 1) return Return(c, CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT);
    if (device != 0) return Return(c, CELL_VIDEO_OUT_ERROR_DEVICE_NOT_FOUND);
    std::lock_guard lock(s.mutex);
    m.Fill(state, 0, 16);
    m.Write<u8>(state + 0, out == CELL_VIDEO_OUT_PRIMARY ? CELL_VIDEO_OUT_OUTPUT_STATE_ENABLED : CELL_VIDEO_OUT_OUTPUT_STATE_DISABLED);
    m.Write<u8>(state + 1, CELL_VIDEO_OUT_COLOR_SPACE_RGB);
    WriteDisplayMode(m, state + 8, CurrentMode(s));
    Return(c, CELL_OK);
}

// cellVideoOutGetResolution(u32 resolutionId, CellVideoOutResolution* {be16 width, be16 height})
void cellVideoOutGetResolution(C& c, M& m, const HleFunction&) {
    const VideoMode* v = FindVideoMode(static_cast<u8>(A32(c, 0)));
    const u32 out = A32(c, 1);
    if (!v || !Writable(m, out, 4)) return Return(c, CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER);
    m.Write<u16>(out, v->width);
    m.Write<u16>(out + 2, v->height);
    Return(c, CELL_OK);
}

// cellVideoOutConfigure(u32 videoOut, CellVideoOutConfiguration* config, CellVideoOutOption* option, u32 waitForEvent)
// CellVideoOutConfiguration (16 bytes): u8 resolutionId, u8 format, u8 aspect, u8 reserved[9], be32 pitch
void cellVideoOutConfigure(C& c, M& m, const HleFunction& f) {
    CellSysutil& s = Self(f);
    const u32 out = A32(c, 0), config = A32(c, 1);
    if (out != CELL_VIDEO_OUT_PRIMARY) return Return(c, CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT);
    if (!config || !m.Check(config, 16, vm::kPageReadable)) return Return(c, CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER);
    const u8 id = m.Read<u8>(config);
    const VideoMode* v = FindVideoMode(id);
    if (!v || !Available(s, *v)) {
        LOG_WARN("cellSysutil", "cellVideoOutConfigure: resolution id 0x%x not offered (max %u lines)", id, s.config().max_height);
        return Return(c, CELL_VIDEO_OUT_ERROR_ILLEGAL_CONFIGURATION);
    }
    std::lock_guard lock(s.mutex);
    s.video_resolution = id;
    s.video_format = m.Read<u8>(config + 1);
    s.video_aspect = m.Read<u8>(config + 2);
    s.video_pitch = m.Read<u32>(config + 12);
    LOG_INFO("cellSysutil", "cellVideoOutConfigure: %ux%u, format %u, aspect %u, pitch %u", v->width, v->height, s.video_format,
             s.video_aspect, s.video_pitch);
    Return(c, CELL_OK);
}

// cellVideoOutGetConfiguration(u32 videoOut, CellVideoOutConfiguration* config, CellVideoOutOption* option)
void cellVideoOutGetConfiguration(C& c, M& m, const HleFunction& f) {
    CellSysutil& s = Self(f);
    const u32 out = A32(c, 0), config = A32(c, 1), option = A32(c, 2);
    if (!Writable(m, config, 16)) return Return(c, CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER);
    if (out > 1) return Return(c, CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT);
    std::lock_guard lock(s.mutex);
    const u8 id = CurrentMode(s);
    m.Fill(config, 0, 16);
    if (out == CELL_VIDEO_OUT_PRIMARY) {
        m.Write<u8>(config + 0, id);
        m.Write<u8>(config + 1, s.video_format);
        m.Write<u8>(config + 2, s.video_aspect ? s.video_aspect : CELL_VIDEO_OUT_ASPECT_16_9);
        m.Write<u32>(config + 12, s.video_pitch ? s.video_pitch : u32{FindVideoMode(id)->width} * 4);
    }
    if (option && m.Check(option, 4, vm::kPageWritable)) m.Write<u32>(option, 0);
    Return(c, CELL_OK);
}

// cellVideoOutGetResolutionAvailability(u32 videoOut, u32 resolutionId, u32 aspect, u32 option)
// Returns 1 (available) or 0 — not an error code.
void cellVideoOutGetResolutionAvailability(C& c, M&, const HleFunction& f) {
    const VideoMode* v = FindVideoMode(static_cast<u8>(A32(c, 1)));
    Return(c, A32(c, 0) == CELL_VIDEO_OUT_PRIMARY && v && Available(Self(f), *v) ? 1 : 0);
}

// cellVideoOutGetNumberOfDevice(u32 videoOut): 1 for the primary output, 0 for the secondary.
void cellVideoOutGetNumberOfDevice(C& c, M&, const HleFunction&) {
    const u32 out = A32(c, 0);
    if (out > 1) return Return(c, CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT);
    Return(c, out == CELL_VIDEO_OUT_PRIMARY ? 1 : 0);
}

// cellVideoOutGetDeviceInfo(u32 videoOut, u32 deviceIndex, CellVideoOutDeviceInfo* info)
// CellVideoOutDeviceInfo (456 bytes): u8 portType, u8 colorSpace, be16 latency, u8 availableModeCount,
// u8 state, u8 rgbOutputRange, u8 reserved[5], CellVideoOutColorInfo colorInfo (20 bytes, at 12),
// CellVideoOutDisplayMode availableModes[32] (at 32), CellVideoOutKSVList ksvList (168 bytes, at 288).
void cellVideoOutGetDeviceInfo(C& c, M& m, const HleFunction& f) {
    CellSysutil& s = Self(f);
    const u32 out = A32(c, 0), device = A32(c, 1), info = A32(c, 2);
    if (!Writable(m, info, 456)) return Return(c, CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER);
    if (out != CELL_VIDEO_OUT_PRIMARY) return Return(c, CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT);
    if (device != 0) return Return(c, CELL_VIDEO_OUT_ERROR_DEVICE_NOT_FOUND);
    m.Fill(info, 0, 456);
    m.Write<u8>(info + 0, CELL_VIDEO_OUT_PORT_HDMI);
    m.Write<u8>(info + 1, CELL_VIDEO_OUT_COLOR_SPACE_RGB);
    m.Write<u16>(info + 2, 0);  // latency (ms)
    u8 count = 0;
    for (const VideoMode& v : kModes)
        if (Available(s, v)) WriteDisplayMode(m, info + 32 + 8u * count++, v.id);
    m.Write<u8>(info + 4, count);
    m.Write<u8>(info + 5, CELL_VIDEO_OUT_DEVICE_STATE_AVAILABLE);
    m.Write<u8>(info + 6, 1);  // CELL_VIDEO_OUT_RGB_OUTPUT_RANGE_FULL
    Return(c, CELL_OK);
}

// ---------------------------------------------------------------------------------------
// Audio out: LPCM stereo 48 kHz
// ---------------------------------------------------------------------------------------

constexpr u32 CELL_AUDIO_OUT_PRIMARY = 0;
constexpr u32 CELL_AUDIO_OUT_CODING_TYPE_LPCM = 0;
constexpr u32 CELL_AUDIO_OUT_FS_48KHZ = 0x04;

// cellAudioOutGetSoundAvailability(u32 audioOut, u32 type, u32 fs, u32 option): max channels (0 = none)
void cellAudioOutGetSoundAvailability(C& c, M&, const HleFunction&) {
    const bool ok = A32(c, 0) == CELL_AUDIO_OUT_PRIMARY && A32(c, 1) == CELL_AUDIO_OUT_CODING_TYPE_LPCM && (A32(c, 2) & CELL_AUDIO_OUT_FS_48KHZ);
    Return(c, ok ? 2 : 0);
}

// cellAudioOutGetSoundAvailability2(u32 audioOut, u32 type, u32 fs, u32 ch, u32 option): 1 / 0
void cellAudioOutGetSoundAvailability2(C& c, M&, const HleFunction&) {
    const bool ok = A32(c, 0) == CELL_AUDIO_OUT_PRIMARY && A32(c, 1) == CELL_AUDIO_OUT_CODING_TYPE_LPCM &&
                    (A32(c, 2) & CELL_AUDIO_OUT_FS_48KHZ) && A32(c, 3) <= 2;
    Return(c, ok ? 1 : 0);
}

// cellAudioOutGetState(u32 audioOut, u32 deviceIndex, CellAudioOutState* state)
// CellAudioOutState (20 bytes): u8 state, u8 encoder, u8 reserved[6], be32 downMixer,
// CellAudioOutSoundMode {u8 type, u8 channel, u8 fs, u8 reserved, be32 layout}
void cellAudioOutGetState(C& c, M& m, const HleFunction&) {
    const u32 out = A32(c, 0), state = A32(c, 2);
    if (!Writable(m, state, 20)) return Return(c, CELL_EFAULT);
    if (out > 1) return Return(c, CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT);
    m.Fill(state, 0, 20);
    m.Write<u8>(state + 0, out == CELL_AUDIO_OUT_PRIMARY ? 0 : 1);  // ENABLED / DISABLED
    m.Write<u8>(state + 12, CELL_AUDIO_OUT_CODING_TYPE_LPCM);
    m.Write<u8>(state + 13, 2);
    m.Write<u8>(state + 14, CELL_AUDIO_OUT_FS_48KHZ);
    m.Write<u32>(state + 16, 1);  // CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH
    Return(c, CELL_OK);
}

// cellAudioOutConfigure(u32 audioOut, CellAudioOutConfiguration* config, CellAudioOutOption* option, u32 waitForEvent)
// CellAudioOutConfiguration (16 bytes): u8 channel, u8 encoder, u8 reserved[10], be32 downMixer
void cellAudioOutConfigure(C& c, M& m, const HleFunction&) {
    const u32 out = A32(c, 0), config = A32(c, 1);
    if (out != CELL_AUDIO_OUT_PRIMARY) return Return(c, CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT);
    if (!config || !m.Check(config, 16, vm::kPageReadable)) return Return(c, CELL_AUDIO_OUT_ERROR_ILLEGAL_CONFIGURATION);
    LOG_INFO("cellSysutil", "cellAudioOutConfigure: %u channel(s), encoder %u (output is stereo LPCM)", m.Read<u8>(config),
             m.Read<u8>(config + 1));
    Return(c, CELL_OK);
}

void cellAudioOutGetConfiguration(C& c, M& m, const HleFunction&) {
    const u32 out = A32(c, 0), config = A32(c, 1);
    if (!Writable(m, config, 16)) return Return(c, CELL_EFAULT);
    if (out > 1) return Return(c, CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT);
    m.Fill(config, 0, 16);
    m.Write<u8>(config, out == CELL_AUDIO_OUT_PRIMARY ? 2 : 0);
    Return(c, CELL_OK);
}

// cellAudioOutGetDeviceInfo(u32 audioOut, u32 deviceIndex, CellAudioOutDeviceInfo* info)
// CellAudioOutDeviceInfo (136 bytes): u8 portType, u8 availableModeCount, u8 state, u8 reserved[3],
// be16 latency, CellAudioOutSoundMode availableModes[16] (8 bytes each, at 8).
void cellAudioOutGetDeviceInfo(C& c, M& m, const HleFunction&) {
    const u32 out = A32(c, 0), device = A32(c, 1), info = A32(c, 2);
    if (!Writable(m, info, 136)) return Return(c, CELL_EFAULT);
    if (out != CELL_AUDIO_OUT_PRIMARY) return Return(c, CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT);
    if (device != 0) return Return(c, CELL_AUDIO_OUT_ERROR_DEVICE_NOT_FOUND);
    m.Fill(info, 0, 136);
    m.Write<u8>(info + 0, 0);  // CELL_AUDIO_OUT_PORT_HDMI
    m.Write<u8>(info + 1, 1);  // one mode
    m.Write<u8>(info + 2, 1);  // CELL_AUDIO_OUT_DEVICE_STATE_AVAILABLE
    m.Write<u16>(info + 6, 0);
    m.Write<u8>(info + 8, CELL_AUDIO_OUT_CODING_TYPE_LPCM);
    m.Write<u8>(info + 9, 2);
    m.Write<u8>(info + 10, CELL_AUDIO_OUT_FS_48KHZ);
    m.Write<u32>(info + 12, 1);  // CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH
    Return(c, CELL_OK);
}

// cellAudioOutSetCopyControl(u32 audioOut, u32 control): copy-protection flag for digital out.
void cellAudioOutSetCopyControl(C& c, M&, const HleFunction&) {
    if (A32(c, 0) > 1) return Return(c, CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT);
    Return(c, CELL_OK);
}

void cellAudioOutGetNumberOfDevice(C& c, M&, const HleFunction&) {
    const u32 out = A32(c, 0);
    if (out > 1) return Return(c, CELL_AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT);
    Return(c, out == CELL_AUDIO_OUT_PRIMARY ? 1 : 0);
}

// ---------------------------------------------------------------------------------------
// Status callbacks
// ---------------------------------------------------------------------------------------

// cellSysutilRegisterCallback(s32 slot, CellSysutilCallback func, void* userdata)
void cellSysutilRegisterCallback(C& c, M&, const HleFunction& f) {
    CellSysutil& s = Self(f);
    const u32 slot = A32(c, 0);
    if (slot > 3) return Return(c, CELL_SYSUTIL_ERROR_VALUE);
    std::lock_guard lock(s.mutex);
    s.callbacks[slot] = {A32(c, 1), A32(c, 2)};
    LOG_INFO("cellSysutil", "callback slot %u: func 0x%08x, userdata 0x%08x", slot, A32(c, 1), A32(c, 2));
    Return(c, CELL_OK);
}

void cellSysutilUnregisterCallback(C& c, M&, const HleFunction& f) {
    CellSysutil& s = Self(f);
    const u32 slot = A32(c, 0);
    if (slot > 3) return Return(c, CELL_SYSUTIL_ERROR_VALUE);
    std::lock_guard lock(s.mutex);
    s.callbacks[slot] = {};
    Return(c, CELL_OK);
}

// Delivers queued statuses: each registered callback gets (status, param, userdata).
void cellSysutilCheckCallback(C& c, M&, const HleFunction& f) {
    CellSysutil& s = Self(f);
    for (;;) {
        std::pair<u32, u64> event;
        CellSysutil::Callback slots[4];
        {
            std::lock_guard lock(s.mutex);
            if (s.pending.empty()) break;
            event = s.pending.front();
            s.pending.pop_front();
            std::copy(std::begin(s.callbacks), std::end(s.callbacks), slots);
        }
        for (const auto& cb : slots)
            if (cb.func) s.kernel().threads().CallGuest3(cb.func, event.first, event.second, cb.userdata, "sysutil callback");
        if (s.kernel().is_terminating()) break;
    }
    Return(c, CELL_OK);
}

// ---------------------------------------------------------------------------------------
// System parameters
// ---------------------------------------------------------------------------------------

// cellSysutilGetSystemParamInt(s32 id, s32* value)
void cellSysutilGetSystemParamInt(C& c, M& m, const HleFunction&) {
    const u32 id = A32(c, 0), out = A32(c, 1);
    if (!Writable(m, out, 4)) return Return(c, CELL_EFAULT);
    s32 value = 0;
    switch (id) {
    case 0x0111: value = 1; break;   // LANG: CELL_SYSUTIL_LANG_ENGLISH_US
    case 0x0112: value = 1; break;   // ENTER_BUTTON_ASSIGN: CROSS
    case 0x0114: value = 1; break;   // DATE_FORMAT: DDMMYYYY
    case 0x0115: value = 1; break;   // TIME_FORMAT: 24-hour clock
    case 0x0116: value = 0; break;   // TIMEZONE (minutes from UTC)
    case 0x0117: value = 0; break;   // SUMMERTIME
    case 0x0121: value = 0; break;   // GAME_PARENTAL_LEVEL: off
    case 0x0123: value = 0; break;   // GAME_PARENTAL_LEVEL0_RESTRICT
    case 0x0141: value = 0; break;   // CURRENT_USER_HAS_NP_ACCOUNT: no
    case 0x0151: value = 0; break;   // CAMERA_PLFREQ
    case 0x0152: value = 1; break;   // PAD_RUMBLE: on
    case 0x0153: value = 0; break;   // KEYBOARD_TYPE
    case 0x0154: value = 0; break;   // JAPANESE_KEYBOARD_ENTRY_METHOD
    case 0x0155: value = 0; break;   // CHINESE_KEYBOARD_ENTRY_METHOD
    case 0x0156: value = 0; break;   // PAD_AUTOOFF
    case 0x0157: value = 0; break;   // MAGNETOMETER
    default:
        LOG_WARN("cellSysutil", "cellSysutilGetSystemParamInt: unknown id 0x%04x", id);
        return Return(c, CELL_SYSUTIL_ERROR_VALUE);
    }
    m.Write<u32>(out, static_cast<u32>(value));
    Return(c, CELL_OK);
}

// cellSysutilGetSystemParamString(s32 id, char* buf, u32 bufsize)
void cellSysutilGetSystemParamString(C& c, M& m, const HleFunction&) {
    const u32 id = A32(c, 0), buf = A32(c, 1), size = A32(c, 2);
    const char* value = nullptr;
    u32 max = 0;
    switch (id) {
    case 0x0113: value = "Player"; max = 128; break;  // NICKNAME
    case 0x0131: value = "Player"; max = 64; break;   // CURRENT_USERNAME
    default:
        LOG_WARN("cellSysutil", "cellSysutilGetSystemParamString: unknown id 0x%04x", id);
        return Return(c, CELL_SYSUTIL_ERROR_VALUE);
    }
    if (size < max) return Return(c, CELL_SYSUTIL_ERROR_SIZE);
    if (!Writable(m, buf, size)) return Return(c, CELL_EFAULT);
    m.Fill(buf, 0, size);
    m.CopyIn(buf, value, std::strlen(value));
    Return(c, CELL_OK);
}

// ---------------------------------------------------------------------------------------
// System cache: /dev_hdd1/caches/<cacheId>
// ---------------------------------------------------------------------------------------

// cellSysCacheMount(CellSysCacheParam* param): char cacheId[32]; char getCachePath[1055]; ...
void cellSysCacheMount(C& c, M& m, const HleFunction& f) {
    CellSysutil& s = Self(f);
    const u32 param = A32(c, 0);
    if (!param || !m.Check(param, 32 + 1055, vm::kPageWritable)) return Return(c, CELL_SYSCACHE_ERROR_PARAM);
    const auto id = m.ReadCString(param, 32);
    if (!id || id->empty() || id->size() >= 32) return Return(c, CELL_SYSCACHE_ERROR_PARAM);
    const std::string guest = "/dev_hdd1/caches/" + *id;
    const auto host = s.kernel().vfs().Resolve(guest);
    if (!host) {
        LOG_ERROR("cellSysutil", "cellSysCacheMount(\"%s\"): /dev_hdd1 is not mounted", id->c_str());
        return Return(c, CELL_SYSCACHE_ERROR_NOTMOUNTED);
    }
    std::error_code ec;
    const bool existed = std::filesystem::is_directory(*host, ec);
    std::filesystem::create_directories(*host, ec);
    if (ec) return Return(c, CELL_SYSCACHE_ERROR_ACCESS_ERROR);
    m.Fill(param + 32, 0, 1055);
    m.CopyIn(param + 32, guest.data(), guest.size());
    {
        std::lock_guard lock(s.mutex);
        s.cache_path = guest;
    }
    LOG_INFO("cellSysutil", "cellSysCacheMount: %s (%s)", guest.c_str(), existed ? "relayed" : "new");
    Return(c, existed ? CELL_SYSCACHE_RET_OK_RELAYED : CELL_SYSCACHE_RET_OK_CLEARED);
}

void cellSysCacheClear(C& c, M&, const HleFunction& f) {
    CellSysutil& s = Self(f);
    std::string guest;
    {
        std::lock_guard lock(s.mutex);
        guest = s.cache_path;
    }
    if (guest.empty()) return Return(c, CELL_SYSCACHE_ERROR_NOTMOUNTED);
    if (const auto host = s.kernel().vfs().Resolve(guest)) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(*host, ec), end; !ec && it != end; it.increment(ec))
            std::filesystem::remove_all(it->path(), ec);
    }
    Return(c, CELL_SYSCACHE_RET_OK_CLEARED);
}

}  // namespace

const VideoMode* FindVideoMode(u8 id) noexcept {
    for (const VideoMode& v : kModes)
        if (v.id == id) return &v;
    return nullptr;
}

CellSysutil::CellSysutil(lv2::Kernel& kernel, Config config) noexcept : kernel_(kernel), config_(config) {}

void CellSysutil::PostEvent(u32 status, u64 param) {
    std::lock_guard lock(mutex);
    pending.emplace_back(status, param);
}

void CellSysutil::Register(HleRegistry& r) {
    constexpr const char* kModule = "cellSysutil";
    struct Entry {
        const char* name;
        HleFunc fn;
    };
    static constexpr Entry kEntries[] = {
        {"cellVideoOutGetState", cellVideoOutGetState},
        {"cellVideoOutGetResolution", cellVideoOutGetResolution},
        {"cellVideoOutConfigure", cellVideoOutConfigure},
        {"cellVideoOutGetConfiguration", cellVideoOutGetConfiguration},
        {"cellVideoOutGetResolutionAvailability", cellVideoOutGetResolutionAvailability},
        {"cellVideoOutGetNumberOfDevice", cellVideoOutGetNumberOfDevice},
        {"cellVideoOutGetDeviceInfo", cellVideoOutGetDeviceInfo},
        {"cellAudioOutGetSoundAvailability", cellAudioOutGetSoundAvailability},
        {"cellAudioOutGetSoundAvailability2", cellAudioOutGetSoundAvailability2},
        {"cellAudioOutGetState", cellAudioOutGetState},
        {"cellAudioOutConfigure", cellAudioOutConfigure},
        {"cellAudioOutGetConfiguration", cellAudioOutGetConfiguration},
        {"cellAudioOutGetNumberOfDevice", cellAudioOutGetNumberOfDevice},
        {"cellAudioOutGetDeviceInfo", cellAudioOutGetDeviceInfo},
        {"cellAudioOutSetCopyControl", cellAudioOutSetCopyControl},
        {"cellSysutilRegisterCallback", cellSysutilRegisterCallback},
        {"cellSysutilUnregisterCallback", cellSysutilUnregisterCallback},
        {"cellSysutilCheckCallback", cellSysutilCheckCallback},
        {"cellSysutilGetSystemParamInt", cellSysutilGetSystemParamInt},
        {"cellSysutilGetSystemParamString", cellSysutilGetSystemParamString},
        {"cellSysCacheMount", cellSysCacheMount},
        {"cellSysCacheClear", cellSysCacheClear},
    };
    for (const Entry& e : kEntries) r.Register(kModule, e.name, e.fn, this);
}

}  // namespace cell::hle
