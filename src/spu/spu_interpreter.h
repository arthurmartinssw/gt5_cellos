// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — spu/spu_interpreter.h
//
// Reference SPU executor: interprets the SPU ISA over the thread's local store and talks to
// the rest of the system only through SpuThread (channels, stop-and-signal, faults).
//
// Role in the translation layer: correctness baseline and fallback. The SPU -> x86-64
// translator plugs into the same ISpuExecutor interface and can be checked instruction by
// instruction against this interpreter.
//
// Model notes:
//   * Registers are kept in SpuContext in architectural big-endian byte order (byte 0 = most
//     significant, preferred slot = bytes 0..3), the LS is big-endian like on hardware.
//   * LS accesses (fetch, lq*, stq*) use relaxed atomics: the PPU (sys_spu_thread_read_ls /
//     write_ls) and peer SPUs (DMA) may touch the LS concurrently.
//   * Single precision follows the host (IEEE, round-to-nearest) with the SPU's flush of
//     denormals to zero and saturation instead of infinities. The hardware truncates and has
//     an extended range; results can differ in the last bit. frest/frsqest return exact
//     reciprocals and fi passes its estimate through (Newton-Raphson steps then converge).
//   * No asynchronous interrupts: bi/iret D/E bits are ignored, bisled tests the event
//     channel synchronously.
#pragma once

#include <atomic>

#include "spu/spu_thread.h"

namespace cell::spu {

class SpuInterpreter final : public ISpuExecutor {
public:
    void Run(SpuThread& thread) override;
    [[nodiscard]] u64 executed() const noexcept override { return executed_; }

private:
    u64 executed_ = 0;
};

// Factory for SpuManager::SetExecutorFactory.
[[nodiscard]] std::unique_ptr<ISpuExecutor> MakeSpuInterpreter(SpuThread& thread);

}  // namespace cell::spu
