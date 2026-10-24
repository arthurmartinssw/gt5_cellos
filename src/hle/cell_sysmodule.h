// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_sysmodule.h
//
// cellSysmodule: games ask lv2 to load system PRX modules (cellFs, cellSpurs, cellRtc...)
// by numeric ID before using them. In this project every system library is HLE: the game's
// imports were already bound to host handlers (or logging stubs) when the EBOOT was loaded,
// so "loading" is bookkeeping. We record the set so cellSysmoduleIsLoaded stays consistent
// and log each ID once with its name, which tells us which library to implement next.
#pragma once

#include <mutex>
#include <set>

#include "hle/hle_registry.h"

namespace cell::hle {

inline constexpr s32 CELL_SYSMODULE_LOADED = 0;
inline constexpr s32 CELL_SYSMODULE_ERROR_DUPLICATED = static_cast<s32>(0x80012001u);
inline constexpr s32 CELL_SYSMODULE_ERROR_UNKNOWN = static_cast<s32>(0x80012002u);
inline constexpr s32 CELL_SYSMODULE_ERROR_UNLOADED = static_cast<s32>(0x80012003u);

class CellSysmodule {
public:
    void Register(HleRegistry& registry);

    s32 Load(u16 id);
    s32 Unload(u16 id);
    [[nodiscard]] bool IsLoaded(u16 id) const;
    [[nodiscard]] static const char* ModuleName(u16 id) noexcept;

private:
    mutable std::mutex mutex_;
    std::set<u16> loaded_;
};

}  // namespace cell::hle
