// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_usbd.h
//
// cellUsbd: the USB driver framework. Games register "LDDs" (logical device drivers: a probe /
// attach / detach callback table) for accessories — for Gran Turismo 5 that means wheels and
// other USB controllers it drives directly. Here no USB device is ever attached, so an LDD's
// probe callback is never called: initialisation and registration succeed and nothing follows,
// exactly what a console with nothing plugged in does.
//   0xD0E766FE cellUsbdInit()                   0x5C832BD7 cellUsbdSetThreadPriority2(e, u, cb)
//   cellUsbdEnd, cellUsbdSetThreadPriority, cellUsbdRegisterLdd / RegisterExtraLdd(2) /
//   UnregisterLdd / UnregisterExtraLdd: CELL_OK.
// Transfers and pipes stay logging stubs: they cannot be reached without an attached device.
#pragma once

#include "hle/hle_registry.h"

namespace cell::hle {

class CellUsbd {
public:
    void Register(HleRegistry& registry);
};

}  // namespace cell::hle
