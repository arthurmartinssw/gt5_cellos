// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — rsx/rsx_methods.h
//
// RSX command-buffer (FIFO) encoding and the handful of method registers the front end
// consumes itself. Values per the NV40-family command format as used by libgcm (and as
// reimplemented by RPCS3's rsx/gcm_enums.h).
//
// A FIFO word is one of:
//   old jump    (w & 0xE0000003) == 0x20000000   target io offset = w & 0x1FFFFFFC
//   new jump    (w & 3) == 1                     target io offset = w & ~3
//   call        (w & 3) == 2                     push return, target io offset = w & ~3
//   return      (w & 0xFFFF0003) == 0x00020000
//   method      (w & 0xE0030003) == 0            count = (w >> 18) & 0x7FF, register = w & 0xFFFC,
//                                                 followed by `count` argument words; the
//                                                 register advances by 4 per argument
//   method, NI  (w & 0xE0030003) == 0x40000000   same, register does not advance
// A zero word is a method with count 0: a NOP.
#pragma once

#include "common/types.h"

namespace cell::rsx {

// Memory the RSX sees.
inline constexpr u32 kLocalMemoryBase = 0xC0000000;  // RSX local (GDDR3) in the PPU address space
inline constexpr u32 kLocalMemoryFull = 0x10000000;  // 256 MiB physically
inline constexpr u32 kLocalMemoryGameSize = 0x0F900000;  // what libgcm reports to games (249 MiB)
inline constexpr u32 kIoPageSize = 0x100000;         // IO (main memory seen by the RSX) is mapped in 1 MiB pages
inline constexpr u32 kIoSpaceSize = 0x10000000;      // 256 MiB of IO address space (default IOMAP mode)
inline constexpr u32 kIoPages = kIoSpaceSize / kIoPageSize;

// Command words
inline constexpr u32 RSX_METHOD_OLD_JUMP_CMD_MASK = 0xE0000003;
inline constexpr u32 RSX_METHOD_OLD_JUMP_CMD = 0x20000000;
inline constexpr u32 RSX_METHOD_OLD_JUMP_OFFSET_MASK = 0x1FFFFFFC;
inline constexpr u32 RSX_METHOD_NEW_JUMP_CMD_MASK = 0x00000003;
inline constexpr u32 RSX_METHOD_NEW_JUMP_CMD = 0x00000001;
inline constexpr u32 RSX_METHOD_CALL_CMD = 0x00000002;
inline constexpr u32 RSX_METHOD_OFFSET_MASK = 0xFFFFFFFC;
inline constexpr u32 RSX_METHOD_RETURN_MASK = 0xFFFF0003;
inline constexpr u32 RSX_METHOD_RETURN_CMD = 0x00020000;
inline constexpr u32 RSX_METHOD_KIND_MASK = 0xE0030003;
inline constexpr u32 RSX_METHOD_INCREMENT_CMD = 0x00000000;
inline constexpr u32 RSX_METHOD_NON_INCREMENT_CMD = 0x40000000;
inline constexpr u32 RSX_METHOD_COUNT_SHIFT = 18;
inline constexpr u32 RSX_METHOD_COUNT_MASK = 0x7FF;
inline constexpr u32 RSX_METHOD_REGISTER_MASK = 0xFFFC;

[[nodiscard]] constexpr u32 MethodHeader(u32 reg, u32 count) noexcept {
    return (count << RSX_METHOD_COUNT_SHIFT) | (reg & RSX_METHOD_REGISTER_MASK);
}

// Method registers (byte addresses) consumed by the front end.
inline constexpr u32 NV406E_SET_REFERENCE = 0x0050;          // CellGcmControl.ref = value
inline constexpr u32 NV406E_SET_CONTEXT_DMA_SEMAPHORE = 0x0060;
inline constexpr u32 NV406E_SEMAPHORE_OFFSET = 0x0064;
inline constexpr u32 NV406E_SEMAPHORE_ACQUIRE = 0x0068;      // wait until label == value
inline constexpr u32 NV406E_SEMAPHORE_RELEASE = 0x006C;      // label = value
inline constexpr u32 NV4097_NO_OPERATION = 0x0100;
inline constexpr u32 NV4097_SET_SEMAPHORE_OFFSET = 0x1D6C;
inline constexpr u32 NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE = 0x1D70;  // label = swizzled value, after rendering
inline constexpr u32 NV4097_TEXTURE_READ_SEMAPHORE_RELEASE = 0x1D74;    // label = value, after texture reads
// Private method of this translation layer: cellGcmSetFlipCommand (HLE) writes it, the front
// end performs the flip. Same register RPCS3 uses for its HLE flip (GCM_FLIP_COMMAND).
inline constexpr u32 kHleFlipMethod = 0xFEAC;
// cellGcmSetUserCommand(ctx, cause): the RSX raises the user interrupt with `cause` when it
// reaches this method (RPCS3 gcm_enums.h GCM_SET_USER_COMMAND = 0xEB00 >> 2).
inline constexpr u32 kGcmSetUserCommand = 0xEB00;

// NV4097 (3D engine) "action" methods, counted by the front end so the reports can say whether
// the game reached a draw or a clear (byte offsets; RPCS3 gcm_enums.h stores them >> 2).
inline constexpr u32 NV4097_SET_BEGIN_END = 0x1808;      // arg = primitive type to begin, 0 = end
inline constexpr u32 NV4097_DRAW_ARRAYS = 0x1814;        // (count - 1) << 24 | first vertex
inline constexpr u32 NV4097_INLINE_ARRAY = 0x1818;       // vertex data inline in the FIFO
inline constexpr u32 NV4097_DRAW_INDEX_ARRAY = 0x1824;   // (count - 1) << 24 | first index
inline constexpr u32 NV4097_CLEAR_SURFACE = 0x1D94;      // mask of the buffers to clear

// Labels (semaphores) are 16 bytes apart; libgcm exposes 256 of them.
inline constexpr u32 kLabelCount = 256;
inline constexpr u32 kLabelStride = 0x10;

}  // namespace cell::rsx
