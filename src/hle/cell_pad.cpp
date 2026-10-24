// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_pad.cpp
#include "hle/cell_pad.h"

#include <algorithm>
#include <atomic>

#include "common/log.h"

namespace cell::hle {

namespace {

using C = ppu::PpuThreadContext;
using M = vm::GuestMemory;

u32 A32(const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); }
void Return(C& c, s32 v) { c.gpr[3] = static_cast<u64>(static_cast<s64>(v)); }
CellPad& Self(const HleFunction& f) { return *static_cast<CellPad*>(f.user); }
bool Writable(const M& m, u32 ea, u32 size) { return ea && m.Check(ea, size, vm::kPageWritable); }

void cellPadInit(C& c, M&, const HleFunction& f) {
    CellPad& s = Self(f);
    const u32 max = A32(c, 0);
    std::lock_guard lock(s.mutex);
    if (s.max_connect) return Return(c, CELL_PAD_ERROR_ALREADY_INITIALIZED);
    if (max == 0 || max > CELL_MAX_PADS) return Return(c, CELL_PAD_ERROR_INVALID_PARAMETER);
    s.max_connect = max;
    s.port_setting.fill(0);  // press and sensor modes off
    LOG_INFO("cellPad", "cellPadInit(%u): no controller connected (host input not mapped yet)", max);
    Return(c, CELL_OK);
}

void cellPadEnd(C& c, M&, const HleFunction& f) {
    CellPad& s = Self(f);
    std::lock_guard lock(s.mutex);
    if (!s.max_connect) return Return(c, CELL_PAD_ERROR_UNINITIALIZED);
    s.max_connect = 0;
    Return(c, CELL_OK);
}

// cellPadGetInfo(CellPadInfo* info): 3 x be32, be16 vendor_id[127], be16 product_id[127], u8 status[127]
void cellPadGetInfo(C& c, M& m, const HleFunction& f) {
    CellPad& s = Self(f);
    std::lock_guard lock(s.mutex);
    if (!s.max_connect) return Return(c, CELL_PAD_ERROR_UNINITIALIZED);
    const u32 info = A32(c, 0);
    if (!Writable(m, info, 647)) return Return(c, CELL_PAD_ERROR_INVALID_PARAMETER);
    m.Fill(info, 0, 647);
    m.Write<u32>(info + 0, s.max_connect);
    Return(c, CELL_OK);
}

// cellPadGetInfo2(CellPadInfo2* info): 3 x be32, then be32 port_status[7], port_setting[7],
// device_capability[7], device_type[7]
void cellPadGetInfo2(C& c, M& m, const HleFunction& f) {
    CellPad& s = Self(f);
    std::lock_guard lock(s.mutex);
    if (!s.max_connect) return Return(c, CELL_PAD_ERROR_UNINITIALIZED);
    const u32 info = A32(c, 0);
    if (!Writable(m, info, 124)) return Return(c, CELL_PAD_ERROR_INVALID_PARAMETER);
    m.Fill(info, 0, 124);
    m.Write<u32>(info + 0, std::min(s.max_connect, CELL_PAD_MAX_PORT_NUM));
    for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; ++i) m.Write<u32>(info + 12 + 28 + 4 * i, s.port_setting[i]);
    Return(c, CELL_OK);
}

// cellPadGetData(port, CellPadData*), cellPadGetCapabilityInfo(port, CellPadCapabilityInfo*):
// no device on any port -> NO_DEVICE, buffer untouched.
void port_with_buffer(C& c, M&, const HleFunction& f) {
    CellPad& s = Self(f);
    std::lock_guard lock(s.mutex);
    if (!s.max_connect) return Return(c, CELL_PAD_ERROR_UNINITIALIZED);
    if (A32(c, 0) >= CELL_MAX_PADS || !A32(c, 1)) return Return(c, CELL_PAD_ERROR_INVALID_PARAMETER);
    Return(c, CELL_PAD_ERROR_NO_DEVICE);
}

// cellPadClearBuf(port)
void cellPadClearBuf(C& c, M&, const HleFunction& f) {
    CellPad& s = Self(f);
    std::lock_guard lock(s.mutex);
    if (!s.max_connect) return Return(c, CELL_PAD_ERROR_UNINITIALIZED);
    if (A32(c, 0) >= CELL_MAX_PADS) return Return(c, CELL_PAD_ERROR_INVALID_PARAMETER);
    Return(c, CELL_PAD_ERROR_NO_DEVICE);
}

// cellPadSetPortSetting(port, setting)
void cellPadSetPortSetting(C& c, M&, const HleFunction& f) {
    CellPad& s = Self(f);
    std::lock_guard lock(s.mutex);
    if (!s.max_connect) return Return(c, CELL_PAD_ERROR_UNINITIALIZED);
    const u32 port = A32(c, 0);
    if (port >= CELL_MAX_PADS) return Return(c, CELL_PAD_ERROR_INVALID_PARAMETER);
    if (port < CELL_PAD_MAX_PORT_NUM) s.port_setting[port] = A32(c, 1) & (CELL_PAD_SETTING_PRESS_ON | CELL_PAD_SETTING_SENSOR_ON);
    Return(c, CELL_OK);
}

// cellPadSetPressMode / cellPadSetSensorMode(port, mode)
template <u32 kBit>
void set_mode(C& c, M&, const HleFunction& f) {
    CellPad& s = Self(f);
    std::lock_guard lock(s.mutex);
    if (!s.max_connect) return Return(c, CELL_PAD_ERROR_UNINITIALIZED);
    const u32 port = A32(c, 0), mode = A32(c, 1);
    if (port >= CELL_MAX_PADS || mode > 1) return Return(c, CELL_PAD_ERROR_INVALID_PARAMETER);
    if (port < CELL_PAD_MAX_PORT_NUM) {
        if (mode) s.port_setting[port] |= kBit;
        else s.port_setting[port] &= ~kBit;
    }
    Return(c, CELL_OK);
}

// ---------------- keyboard / mouse: no device connected ----------------
template <u32 CellPad::*kField, s32 kAlready, s32 kInvalid, u32 kMax>
void device_init(C& c, M&, const HleFunction& f) {
    CellPad& s = Self(f);
    const u32 max = A32(c, 0);
    std::lock_guard lock(s.mutex);
    if (s.*kField) return Return(c, kAlready);
    if (max == 0 || max > kMax) return Return(c, kInvalid);
    s.*kField = std::min<u32>(max, 7);
    LOG_INFO("sys_io", "%s(%u): no device connected", f.name.c_str(), max);
    Return(c, CELL_OK);
}

template <u32 CellPad::*kField, s32 kUninitialized>
void device_end(C& c, M&, const HleFunction& f) {
    CellPad& s = Self(f);
    std::lock_guard lock(s.mutex);
    if (!(s.*kField)) return Return(c, kUninitialized);
    s.*kField = 0;
    Return(c, CELL_OK);
}

// GetInfo: zeroed structure + max_connect; now_connect, info and every per-device entry 0.
template <u32 CellPad::*kField, s32 kUninitialized, s32 kInvalid, u32 kSize>
void device_get_info(C& c, M& m, const HleFunction& f) {
    CellPad& s = Self(f);
    std::lock_guard lock(s.mutex);
    if (!(s.*kField)) return Return(c, kUninitialized);
    const u32 info = A32(c, 0);
    if (!Writable(m, info, kSize)) return Return(c, kInvalid);
    m.Fill(info, 0, kSize);
    m.Write<u32>(info + 0, s.*kField);
    Return(c, CELL_OK);
}

// cellKey2CharOpen(CellKey2CharHandle*): checks as RPCS3 (null -> INVALID_HANDLE; byte 8 of
// the handle non-zero -> ALREADY_INITIALIZED); the handle is not written (see the header).
void cellKey2CharOpen(C& c, M& m, const HleFunction&) {
    const u32 handle = A32(c, 0);
    if (!handle || !m.Check(handle, 128, vm::kPageReadable)) return Return(c, CELL_K2C_ERROR_INVALID_HANDLE);
    if (m.Read<u8>(handle + 8)) return Return(c, CELL_K2C_ERROR_ALREADY_INITIALIZED);
    LOG_WARN("cellKey2char", "cellKey2CharOpen(0x%08x): handle left as given (opaque 128-byte layout unknown; RPCS3 writes nothing "
                             "either); no keyboard is connected", handle);
    Return(c, CELL_OK);
}

}  // namespace

void CellPad::Register(HleRegistry& r) {
    struct Entry {
        const char* name;
        HleFunc fn;
    };
    static constexpr Entry kEntries[] = {
        {"cellPadInit", cellPadInit},
        {"cellPadEnd", cellPadEnd},
        {"cellPadGetInfo", cellPadGetInfo},
        {"cellPadGetInfo2", cellPadGetInfo2},
        {"cellPadGetData", port_with_buffer},
        {"cellPadGetCapabilityInfo", port_with_buffer},
        {"cellPadClearBuf", cellPadClearBuf},
        {"cellPadSetPortSetting", cellPadSetPortSetting},
        {"cellPadSetPressMode", set_mode<CELL_PAD_SETTING_PRESS_ON>},
        {"cellPadSetSensorMode", set_mode<CELL_PAD_SETTING_SENSOR_ON>},
    };
    for (const Entry& e : kEntries) r.Register("sys_io", e.name, e.fn, this);

    using P = CellPad;
    static constexpr Entry kInput[] = {
        {"cellKbInit", device_init<&P::kb_max_connect, CELL_KB_ERROR_ALREADY_INITIALIZED, CELL_KB_ERROR_INVALID_PARAMETER, CELL_KB_MAX_KEYBOARDS>},
        {"cellKbEnd", device_end<&P::kb_max_connect, CELL_KB_ERROR_UNINITIALIZED>},
        {"cellKbGetInfo", device_get_info<&P::kb_max_connect, CELL_KB_ERROR_UNINITIALIZED, CELL_KB_ERROR_INVALID_PARAMETER, kCellKbInfoSize>},
        {"cellMouseInit", device_init<&P::mouse_max_connect, CELL_MOUSE_ERROR_ALREADY_INITIALIZED, CELL_MOUSE_ERROR_INVALID_PARAMETER, CELL_MAX_MICE>},
        {"cellMouseEnd", device_end<&P::mouse_max_connect, CELL_MOUSE_ERROR_UNINITIALIZED>},
        {"cellMouseGetInfo",
         device_get_info<&P::mouse_max_connect, CELL_MOUSE_ERROR_UNINITIALIZED, CELL_MOUSE_ERROR_INVALID_PARAMETER, kCellMouseInfoSize>},
    };
    for (const Entry& e : kInput) r.Register("sys_io", e.name, e.fn, this);
    r.Register("cellKey2char", "cellKey2CharOpen", cellKey2CharOpen, this);
}

}  // namespace cell::hle
