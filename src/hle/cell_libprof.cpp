// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_libprof.cpp
#include "hle/cell_libprof.h"

#include "common/log.h"

namespace cell::hle {

namespace {
// Accepts the call: no profiler is attached, so there is nothing to register with.
void accept_trace(ppu::PpuThreadContext& c, vm::GuestMemory&, const HleFunction& self) {
    LOG_TRACE("cellLibprof", "%s(0x%08x, 0x%08x): no profiler attached, accepted", self.name.c_str(),
              static_cast<u32>(c.gpr[3]), static_cast<u32>(c.gpr[4]));
    c.gpr[3] = CELL_OK;
}
}  // namespace

void CellLibprof::Register(HleRegistry& r) {
    for (const char* name : {"cellUserTraceInit", "cellUserTraceRegister", "cellUserTraceUnregister", "cellUserTraceTerminate"})
        r.Register("cellLibprof", name, accept_trace, this);
}

}  // namespace cell::hle
