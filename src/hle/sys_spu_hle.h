// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/sys_spu_hle.h
//
// The SPU half of sysPrxForUser (liblv2 user-side helpers), registered under that module:
//   sys_spu_image_import(sys_spu_image_t* img, u32 elf, u32 type)
//       type SYS_SPU_IMAGE_PROTECT: the loadable bytes of the embedded SPU ELF are copied to a
//       private block, so the game may overwrite/free the ELF afterwards;
//       type SYS_SPU_IMAGE_DIRECT: COPY segments reference the ELF in place.
//       Either way img->segs points to a sys_spu_segment_t array that lives at the start of one
//       guest allocation in the main area (followed by the copied bytes for PROTECT), so
//       sys_spu_image_close frees exactly one block.
//   sys_spu_image_close, sys_spu_elf_get_information, sys_spu_elf_get_segments
//   _sys_spu_printf_initialize(agcb, dgcb, atcb, dtcb): stores four guest callbacks (OPDs)
//       registered by the game's spu_printf server; _sys_spu_printf_attach/detach_group/thread
//       (called by libsre for the SPURS kernel group, among others) call the matching callback
//       with the group/thread id and return its result. The callback is what connects the SPU
//       group's event port to the spu_printf server's event queue. CELL_ESTAT before
//       initialization, as in lv2's liblv2.
//   sys_raw_spu_load / sys_raw_spu_image_load: raw SPUs are not supported -> explicit abort
#pragma once

#include <atomic>

#include "hle/hle_registry.h"
#include "lv2/kernel.h"

namespace cell::hle {

class SysSpuHle {
public:
    explicit SysSpuHle(lv2::Kernel& kernel) noexcept : kernel_(kernel) {}
    void Register(HleRegistry& registry);
    lv2::Kernel& kernel() noexcept { return kernel_; }

    // spu_printf callbacks (guest OPD addresses); 0 = not initialized.
    enum Callback { kAttachGroup, kDetachGroup, kAttachThread, kDetachThread, kCallbackCount };
    std::atomic<u32> spu_printf[kCallbackCount]{};
    std::atomic<bool> spu_printf_initialized{false};

private:
    lv2::Kernel& kernel_;
};

}  // namespace cell::hle
