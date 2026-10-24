// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/gcm_interrupts.h
//
// libgcm's interrupt handlers: cellGcmSetVBlankHandler / SetFlipHandler / SetQueueHandler /
// SetUserHandler register guest functions (OPDs, void handler(u32)) that the system calls when
// the corresponding RSX/display event happens.
//
// On the console the handlers run on a PPU *interrupt thread* that libgcm creates in
// _cellGcmInitBody (RPCS3: sys_ppu_thread_create(prio 1, stack 0x4000,
// SYS_PPU_THREAD_CREATE_INTERRUPT, "_gcm_intr_thread")), never on the RSX itself. RPCS3 delivers
// (verified in RSXThread.cpp / rsx_methods.cpp):
//   vblank  every vertical blank (60 Hz by default), argument 1 (head)   post_vblank_event
//   flip    after a flip completed and flip status became DONE, arg 1    handle_emu_flip
//   queue   when the RSX reaches a FIFO flip command, arg 1              flip_command
//   user    when the RSX reaches cellGcmSetUserCommand, arg = cause      user_command
// cellGcmSetSecondVHandler and cellGcmSetGraphicsHandler are only logged by RPCS3 (todo); we do
// not deliver them either (CellGcmSys records them and says so).
//
// PC mapping. One guest PPU thread "_gcm_intr_thread", created at _cellGcmInitBody like the
// real one, runs a 16-instruction guest trampoline:
//     loop: r3, r4 = gcm_interrupt_wait()      HLE call: blocks on the host until an event
//           if r3 == 0: return                 0 = shut down -> thread exits
//           r2 = opd->toc; ctr = opd->code; r3 = r4; bctrl     the guest handler, guest ABI
//           b loop
// so guest handlers execute only on a PPU thread with its own stack and TLS (r13), through the
// same interpreter as every other guest function — the project's execution model. The host
// side (this object) only queues events: the RSX thread calls OnFlipCommand / OnFlipDone /
// OnUserCommand; vblank deadlines are computed by the waiting interrupt thread itself from the
// same clock as cellGcmGetVBlankCount (no extra host thread).
//
// Semantics chosen where the references are silent, kept deliberately simple:
//   * The handler is captured when the event happens; an event that finds no handler is
//     counted (no_handler) and dropped. Setting a handler to 0 disables it: events already
//     queued for it are dropped at delivery.
//   * Flip / queue / user events are delivered one call per event, in order (bounded queue of
//     64; overflow is counted). Vblanks are an interrupt *level*: if the thread is late by k
//     vblanks it runs the handler once and counts k-1 as missed.
//   * The first vblank after a handler is installed is the next vblank boundary (no burst).
// Lifetime: the object lives in the kernel object table (so process termination wakes the
// waiting thread through WakeForTermination) and therefore outlives CellGcmSys; Close() makes
// the thread leave its loop.
#pragma once

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

#include "lv2/kernel.h"
#include "rsx/rsx_fifo.h"

namespace cell::hle {

enum class GcmEvent : u8 { VBlank = 0, Flip = 1, Queue = 2, User = 3 };
inline constexpr std::size_t kGcmEventKinds = 4;
[[nodiscard]] const char* GcmEventName(GcmEvent kind) noexcept;

// Runtime object id range in the kernel object table (not an lv2 object type the guest uses).
inline constexpr u32 kIdGcmInterrupts = 0x7E000000;

class GcmInterrupts final : public lv2::Lv2Object, public rsx::IRsxEventSink {
public:
    static constexpr std::size_t kMaxQueued = 64;
    static constexpr u32 kTrampolineWords = 16;

    explicit GcmInterrupts(lv2::Kernel& kernel) noexcept : kernel_(kernel) {}

    // cellGcmSet*Handler: opd 0 disables.
    void SetHandler(GcmEvent kind, u32 opd);
    [[nodiscard]] u32 handler(GcmEvent kind) const;

    // Writes the trampoline (calls the HLE wait whose thunk code is at `wait_code`) and creates +
    // starts "_gcm_intr_thread". Returns false (and logs) if guest memory or the thread could
    // not be created. Idempotent.
    bool StartThread(u32 wait_code);
    [[nodiscard]] u32 thread_id() const;
    // The trampoline words for code at `code_ea` calling `wait_code` (exposed for tests).
    [[nodiscard]] static std::array<u32, kTrampolineWords> Trampoline(u32 wait_code);

    // rsx::IRsxEventSink — RSX thread.
    void OnFlipCommand(u32 buffer_id) override;
    void OnFlipDone(u32 buffer_id) override;
    void OnUserCommand(u32 cause) override;

    // The HLE wait, on the interrupt thread: blocks until a handler is due and returns its OPD
    // and argument; {0, 0} = leave the loop (Close() or process termination).
    std::pair<u32, u64> WaitNext();
    void Close();

    // Clock shared with cellGcmGetVBlankCount: microseconds since the first call.
    [[nodiscard]] static u64 Now() noexcept;
    [[nodiscard]] static u64 VBlankIndex(u64 now_us) noexcept { return now_us * 60 / 1'000'000; }
    // First microsecond at which VBlankIndex() reaches `index`.
    [[nodiscard]] static u64 VBlankTime(u64 index) noexcept { return (index * 1'000'000 + 59) / 60; }

    struct Counters {
        std::array<u64, kGcmEventKinds> delivered{}, no_handler{}, disabled{}, overflow{};
        u64 vblank_missed = 0;
        std::size_t queued = 0;
    };
    [[nodiscard]] Counters counters() const;

    // lv2::Lv2Object
    void WakeForTermination() override;
    std::string Describe() override;

private:
    void Post(GcmEvent kind, u64 arg);

    struct Pending {
        GcmEvent kind;
        u64 arg;
    };
    lv2::Kernel& kernel_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    // ---- guarded by m_ ----
    std::array<u32, kGcmEventKinds> handlers_{};
    std::deque<Pending> queue_;
    u64 last_vblank_ = 0;  // VBlankIndex already delivered (or current when the handler was set)
    bool changed_ = false; // handler change: the waiter recomputes its deadline
    bool closed_ = false;
    bool waiting_ = false;
    u32 thread_id_ = 0;
    u32 code_ea_ = 0;
    u64 last_delivery_us_ = 0;
    Counters counters_;
};

}  // namespace cell::hle
