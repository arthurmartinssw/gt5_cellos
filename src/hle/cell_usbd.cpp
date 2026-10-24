// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_usbd.cpp
#include "hle/cell_usbd.h"

#include "common/log.h"

namespace cell::hle {

namespace {
void no_devices(ppu::PpuThreadContext& c, vm::GuestMemory&, const HleFunction& f) {
    LOG_INFO("cellUsbd", "%s: accepted (no USB devices are attached)", f.name.c_str());
    c.gpr[3] = CELL_OK;
}
}  // namespace

void CellUsbd::Register(HleRegistry& r) {
    for (const char* name : {"cellUsbdInit", "cellUsbdEnd", "cellUsbdSetThreadPriority", "cellUsbdSetThreadPriority2",
                             "cellUsbdRegisterLdd", "cellUsbdRegisterExtraLdd", "cellUsbdRegisterExtraLdd2", "cellUsbdUnregisterLdd",
                             "cellUsbdUnregisterExtraLdd"})
        r.Register("cellUsbd", name, no_devices, this);
}

}  // namespace cell::hle
