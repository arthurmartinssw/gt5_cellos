// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_pad.h
//
// cellPad (import library "sys_io"), answering as a console with no controller connected.
// GT5 calls cellPadInit(7) at boot. Left as a logging stub, every later cellPadGetData /
// cellPadGetInfo2 would also return CELL_OK with an unwritten buffer — i.e. report a connected
// pad with garbage buttons. Implemented instead with the SDK contract as reproduced by RPCS3
// (cellPad.h/.cpp, Emu/Io/pad_types.h), with zero devices:
//   cellPadInit(u32 max_connect)      ALREADY_INITIALIZED; INVALID_PARAMETER if 0 or > 127
//   cellPadEnd()                      UNINITIALIZED if not initialized
//   cellPadGetInfo(CellPadInfo*)      647 bytes: max_connect, now_connect 0, system_info 0,
//                                     vendor_id[127], product_id[127], status[127] all 0
//   cellPadGetInfo2(CellPadInfo2*)    124 bytes: max_connect (clamped to 7), now_connect 0,
//                                     system_info 0, port_status[7] 0, port_setting[7] (stored),
//                                     device_capability[7] 0, device_type[7] 0
//   cellPadGetData(port, CellPadData*) / cellPadClearBuf(port) / cellPadGetCapabilityInfo(port, info*)
//                                     INVALID_PARAMETER port >= 127 (or null buffer);
//                                     NO_DEVICE otherwise — the buffer is not written
//   cellPadSetPortSetting(port, setting)   stored for ports < 7 (PRESS_ON 0x2 | SENSOR_ON 0x4)
//   cellPadSetPressMode / cellPadSetSensorMode(port, mode)  INVALID_PARAMETER mode > 1;
//                                     stored (applies once a controller is connected)
// Every function except Init returns UNINITIALIZED before cellPadInit / after cellPadEnd.
// Host controller input (XInput / SDL) plugs in here later; until then the game sees no pad.
//
// Keyboard and mouse (same import library), phase 7.7 — GT5 calls cellKbInit (0x433F6EC0) and
// cellMouseInit (0xC9030138) at boot. Contract per RPCS3 cellKb.cpp / cellMouse.cpp and their
// null handlers (Emu/Io/Null/*), i.e. what a console with no keyboard / mouse answers:
//   cellKbInit(u32 max_connect) / cellMouseInit(u32 max_connect)
//                                     ALREADY_INITIALIZED; INVALID_PARAMETER if 0 or > 127;
//                                     stores min(max_connect, 7)
//   cellKbEnd() / cellMouseEnd()      UNINITIALIZED if not initialized
//   cellKbGetInfo(CellKbInfo*)        139 bytes: be32 max_connect, now_connect 0, info 0,
//                                     u8 status[127] 0 (disconnected)
//   cellMouseGetInfo(CellMouseInfo*)  647 bytes: be32 max_connect, now_connect 0, info 0,
//                                     be16 vendor_id[127], be16 product_id[127], u8 status[127] 0
//   GetInfo: UNINITIALIZED first, then INVALID_PARAMETER for a null (here: unwritable) pointer.
// cellKey2CharOpen (import library "cellKey2char", 0xABF629C1): RPCS3 checks the handle (null ->
// INVALID_HANDLE, byte 8 set -> ALREADY_INITIALIZED) and returns CELL_OK without writing the
// 128-byte opaque handle; its layout is not documented anywhere we can verify, so we do exactly
// that and say so in the log. Only Open is implemented (the only key2char call GT5 makes so far).
#pragma once

#include <array>
#include <atomic>
#include <mutex>

#include "hle/hle_registry.h"

namespace cell::hle {

inline constexpr s32 CELL_PAD_ERROR_FATAL = static_cast<s32>(0x80121101u);
inline constexpr s32 CELL_PAD_ERROR_INVALID_PARAMETER = static_cast<s32>(0x80121102u);
inline constexpr s32 CELL_PAD_ERROR_ALREADY_INITIALIZED = static_cast<s32>(0x80121103u);
inline constexpr s32 CELL_PAD_ERROR_UNINITIALIZED = static_cast<s32>(0x80121104u);
inline constexpr s32 CELL_PAD_ERROR_NO_DEVICE = static_cast<s32>(0x80121107u);
inline constexpr u32 CELL_MAX_PADS = 127;
inline constexpr u32 CELL_PAD_MAX_PORT_NUM = 7;
inline constexpr u32 CELL_PAD_SETTING_PRESS_ON = 0x2;
inline constexpr u32 CELL_PAD_SETTING_SENSOR_ON = 0x4;

inline constexpr s32 CELL_KB_ERROR_INVALID_PARAMETER = static_cast<s32>(0x80121002u);
inline constexpr s32 CELL_KB_ERROR_ALREADY_INITIALIZED = static_cast<s32>(0x80121003u);
inline constexpr s32 CELL_KB_ERROR_UNINITIALIZED = static_cast<s32>(0x80121004u);
inline constexpr s32 CELL_MOUSE_ERROR_INVALID_PARAMETER = static_cast<s32>(0x80121202u);
inline constexpr s32 CELL_MOUSE_ERROR_ALREADY_INITIALIZED = static_cast<s32>(0x80121203u);
inline constexpr s32 CELL_MOUSE_ERROR_UNINITIALIZED = static_cast<s32>(0x80121204u);
inline constexpr s32 CELL_K2C_ERROR_INVALID_HANDLE = static_cast<s32>(0x80121302u);
inline constexpr s32 CELL_K2C_ERROR_ALREADY_INITIALIZED = static_cast<s32>(0x80121304u);
inline constexpr u32 CELL_KB_MAX_KEYBOARDS = 127;
inline constexpr u32 CELL_MAX_MICE = 127;
inline constexpr u32 kCellKbInfoSize = 12 + CELL_KB_MAX_KEYBOARDS;               // 139
inline constexpr u32 kCellMouseInfoSize = 12 + 2 * 2 * CELL_MAX_MICE + CELL_MAX_MICE;  // 647

class CellPad {
public:
    void Register(HleRegistry& registry);

    std::mutex mutex;
    u32 max_connect = 0;  // 0 = not initialized (guarded by mutex)
    std::array<u32, CELL_PAD_MAX_PORT_NUM> port_setting{};
    u32 kb_max_connect = 0;     // cellKb: 0 = not initialized, else min(max_connect, 7)
    u32 mouse_max_connect = 0;  // cellMouse: same
};

}  // namespace cell::hle
