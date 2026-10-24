// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_sysmodule.cpp
#include "hle/cell_sysmodule.h"

#include "common/log.h"
#include "ppu/ppu_context.h"

namespace cell::hle {

const char* CellSysmodule::ModuleName(u16 id) noexcept {
    // Low IDs from the SDK's cell/sysmodule.h as commonly documented (RPCS3's cellSysmodule.cpp
    // has the full table). IDs above 0x23 are printed as numbers until verified.
    static constexpr const char* kNames[] = {
        "NET",      "HTTP",    "HTTP_UTIL", "SSL",        "HTTPS",   "VDEC",    "ADEC",   "DMUX",
        "VPOST",    "RTC",     "SPURS",     "OVIS",       "SHEAP",   "SYNC",    "FS",     "JPGDEC",
        "GCM_SYS",  "AUDIO",   "PAMF",      "ATRAC3PLUS", "NETCTL",  "SYSUTIL", "SYSUTIL_NP", "IO",
        "PNGDEC",   "FONT",    "FONTFT",    "FREETYPE",   "USBD",    "SAIL",    "L10N",   "RESC",
        "CELPENC",  "PNGENC",  "JPGENC",    "SYNC2",
    };
    return id < std::size(kNames) ? kNames[id] : nullptr;
}

s32 CellSysmodule::Load(u16 id) {
    bool first;
    {
        std::lock_guard lock(mutex_);
        first = loaded_.insert(id).second;
    }
    if (first) {
        const char* name = ModuleName(id);
        LOG_INFO("cellSysmodule", "load module 0x%04x (%s): HLE, reported as loaded", id, name ? name : "unidentified");
    }
    return CELL_OK;
}

s32 CellSysmodule::Unload(u16 id) {
    std::lock_guard lock(mutex_);
    return loaded_.erase(id) ? CELL_OK : CELL_SYSMODULE_ERROR_UNLOADED;
}

bool CellSysmodule::IsLoaded(u16 id) const {
    std::lock_guard lock(mutex_);
    return loaded_.count(id) != 0;
}

namespace {
using C = ppu::PpuThreadContext;
CellSysmodule& Self(const HleFunction& f) { return *static_cast<CellSysmodule*>(f.user); }
void Return(C& c, s32 v) { c.gpr[3] = static_cast<u64>(static_cast<s64>(v)); }
u16 ModuleId(const C& c) { return static_cast<u16>(c.gpr[3]); }
}  // namespace

void CellSysmodule::Register(HleRegistry& r) {
    constexpr const char* kModule = "cellSysmodule";
    r.Register(kModule, "cellSysmoduleInitialize", [](C& c, vm::GuestMemory&, const HleFunction&) { Return(c, CELL_OK); }, this);
    r.Register(kModule, "cellSysmoduleFinalize", [](C& c, vm::GuestMemory&, const HleFunction&) { Return(c, CELL_OK); }, this);
    r.Register(kModule, "cellSysmoduleSetMemcontainer",
               [](C& c, vm::GuestMemory&, const HleFunction&) { Return(c, CELL_OK); }, this);
    r.Register(kModule, "cellSysmoduleLoadModule",
               [](C& c, vm::GuestMemory&, const HleFunction& f) { Return(c, Self(f).Load(ModuleId(c))); }, this);
    r.Register(kModule, "cellSysmoduleUnloadModule",
               [](C& c, vm::GuestMemory&, const HleFunction& f) { Return(c, Self(f).Unload(ModuleId(c))); }, this);
    r.Register(kModule, "cellSysmoduleIsLoaded", [](C& c, vm::GuestMemory&, const HleFunction& f) {
        Return(c, Self(f).IsLoaded(ModuleId(c)) ? CELL_SYSMODULE_LOADED : CELL_SYSMODULE_ERROR_UNLOADED);
    }, this);
}

}  // namespace cell::hle
