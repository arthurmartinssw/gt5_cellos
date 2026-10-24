// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/net_offline.cpp
#include "hle/net_offline.h"

#include "common/log.h"

namespace cell::hle {

namespace {

using C = ppu::PpuThreadContext;
using M = vm::GuestMemory;

u32 A32(const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); }
void Return(C& c, s32 v) { c.gpr[3] = static_cast<u64>(static_cast<s64>(v)); }

void ok(C& c, M&, const HleFunction& f) {
    LOG_TRACE("net", "%s: offline, accepted", f.name.c_str());
    Return(c, CELL_OK);
}

void not_connected(C& c, M&, const HleFunction& f) {
    LOG_TRACE("net", "%s: not connected", f.name.c_str());
    Return(c, CELL_NET_CTL_ERROR_NOT_CONNECTED);
}

// Writes a 32-bit value through the pointer argument `index` (EFAULT if not writable).
template <int kIndex, s32 kValue>
void out_value(C& c, M& m, const HleFunction&) {
    const u32 out = A32(c, kIndex);
    if (!out || !m.Check(out, 4, vm::kPageWritable)) return Return(c, CELL_EFAULT);
    m.Write<u32>(out, static_cast<u32>(kValue));
    Return(c, CELL_OK);
}

}  // namespace

void NetOffline::Register(HleRegistry& r) {
    struct Entry {
        const char* module;
        const char* name;
        HleFunc fn;
    };
    static constexpr Entry kEntries[] = {
        {"cellNetCtl", "cellNetCtlInit", ok},
        {"cellNetCtl", "cellNetCtlTerm", ok},
        {"cellNetCtl", "cellNetCtlGetState", out_value<0, CELL_NET_CTL_STATE_Disconnected>},
        {"cellNetCtl", "cellNetCtlGetInfo", not_connected},
        {"cellNetCtl", "cellNetCtlGetNatInfo", not_connected},
        {"cellNetCtl", "cellNetCtlAddHandler", out_value<2, 1>},  // (handler, arg, s32* hid)
        {"cellNetCtl", "cellNetCtlDelHandler", ok},
        {"sceNp", "sceNpInit", ok},
        {"sceNp", "sceNpTerm", ok},
        {"sceNp", "sceNpManagerGetStatus", out_value<0, SCE_NP_MANAGER_STATUS_OFFLINE>},
        {"sceNp", "sceNpManagerRegisterCallback", ok},
        {"sceNp", "sceNpManagerUnregisterCallback", ok},
        {"sys_net", "sys_net_initialize_network_ex", ok},
        {"sys_net", "sys_net_finalize_network", ok},
    };
    for (const Entry& e : kEntries) r.Register(e.module, e.name, e.fn, this);
}

}  // namespace cell::hle
