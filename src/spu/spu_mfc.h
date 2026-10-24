// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — spu/spu_mfc.h
//
// Memory Flow Controller: DMA between an SPU's local store and the rest of the system.
//
// Execution model: every command runs to completion when the SPU writes MFC_Cmd. Real MFCs
// queue up to 16 commands and finish them asynchronously; finishing them immediately is a
// legal schedule (tag-group status simply reports "complete" at once) and avoids an extra host
// thread per SPU. The JIT can later batch transfers without changing this interface.
//
// Address resolution for the EA side:
//   * 0xF0000000 + n * 1 MiB + off (off < 256 KiB): LS of SPU thread n of the *same group*
//     (lv2 SYS_SPU_THREAD_BASE_LOW mapping, used for SPU-to-SPU DMA), and +0x5400C / +0x5C00C
//     are its signal-notification registers (SNDSIG).
//   * anything else: the guest arena, page-checked (readable for GET, writable for PUT);
//     PUTs into translated PPU code invalidate it.
//
// Atomic commands (GETLLAR / PUTLLC / PUTLLUC) use a 128-byte reservation guarded by one of
// 256 striped host mutexes keyed by the line address. SPU-side atomics are therefore exact
// against each other. Known gap until the JIT phase: PPU stores do not take the stripe, so a
// PPU write racing a PUTLLC on the same line is not detected.
#pragma once

#include "spu/spu_thread.h"

namespace cell::spu {

// Executes the command described by thread.mfc (lsa/eal/size/tag) with opcode `cmd`.
// Returns false after calling thread.Fault() on an invalid command.
bool MfcExecute(SpuThread& thread, u32 cmd);

// True if the line reserved by GETLLAR changed since (for SPU_EVENT_LR).
[[nodiscard]] bool MfcReservationLost(SpuThread& thread);

}  // namespace cell::spu
