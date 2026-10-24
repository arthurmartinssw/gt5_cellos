// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/net_offline.h
//
// The network libraries, answering as a console with no network cable and no PSN sign-in.
// That is a state every PS3 game handles (it must boot offline), unlike "CELL_OK with an
// unfilled output", which is what a logging stub would give:
//   cellNetCtl  Init/Term OK; GetState -> CELL_NET_CTL_STATE_Disconnected (0);
//               GetInfo / GetNatInfo -> CELL_NET_CTL_ERROR_NOT_CONNECTED; Add/DelHandler OK.
//   sceNp       Init/Term OK; sceNpManagerGetStatus -> SCE_NP_MANAGER_STATUS_OFFLINE (-1);
//               Register/UnregisterCallback OK.
//   sys_net     sys_net_initialize_network_ex / sys_net_finalize_network OK. Sockets stay
//               stubs: a game that saw "disconnected" does not open any.
#pragma once

#include "hle/hle_registry.h"

namespace cell::hle {

inline constexpr s32 CELL_NET_CTL_STATE_Disconnected = 0;
inline constexpr s32 CELL_NET_CTL_ERROR_NOT_CONNECTED = static_cast<s32>(0x80130106u);
inline constexpr s32 SCE_NP_MANAGER_STATUS_OFFLINE = -1;

class NetOffline {
public:
    void Register(HleRegistry& registry);
};

}  // namespace cell::hle
