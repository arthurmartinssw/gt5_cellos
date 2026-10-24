// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_libprof.h
//
// cellLibprof: the user-trace side of the SDK profiler (libprof). libsre registers its SPURS
// trace buffers here so a Performance Analyzer connected to a devkit can read them. On a
// retail console nothing listens and the calls are bookkeeping; libsre only checks for
// CELL_OK. Identified by FNID (SHA-1 of the name, see hle/fnid.h):
//   0x0252EFCC cellUserTraceInit       0x05893E7C cellUserTraceRegister
//   0x6D045C2E cellUserTraceUnregister 0x898C77BF cellUserTraceTerminate
#pragma once

#include "hle/hle_registry.h"

namespace cell::hle {

class CellLibprof {
public:
    void Register(HleRegistry& registry);
};

}  // namespace cell::hle
