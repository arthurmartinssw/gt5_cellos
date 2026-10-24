// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — rsx/rsx_fifo.h
//
// RSX front end: one host thread ("RSX") that plays the role of the RSX command processor.
//
// The PPU side (game code + libgcm inlines) writes commands into the command buffer in IO
// memory and publishes progress by storing an IO offset into CellGcmControl.put. The real RSX
// fetches from CellGcmControl.get up to put, updating get as it goes. This thread does the
// same over guest memory:
//   * control flow: old/new jumps, call/return, NOPs;
//   * synchronization the PPU waits on: SET_REFERENCE (CellGcmControl.ref, used by
//     cellGcmFinish), semaphore acquire/release on the label area, back-end/texture semaphore
//     releases, and flips (flip status, current display buffer, flip count);
//   * every other method goes to the IRsxBackend (null now, Vulkan later), in order;
//   * libgcm's interrupt sources reach the IRsxEventSink: a FIFO flip command (queue
//     handler), every completed flip (flip handler), cellGcmSetUserCommand (user handler).
// Draws (NV4097_SET_BEGIN_END...), clears, flips and put advances are counted (RsxStats) and
// the first of each is logged once — decoding only: nothing is rasterized or presented yet.
// Guest words are accessed with relaxed atomics (big-endian), get/put with acquire/release.
//
// IO translation: the RSX addresses main memory through IO offsets mapped in 1 MiB pages
// (cellGcmInit / cellGcmMapMainMemory / cellGcmMapEaIoAddress). The map lives here so the
// thread can translate without locks; cellGcmSys keeps the guest-visible offset tables.
//
// Errors in the command stream (jump to an unmapped IO offset, invalid command word, label
// out of range) end the process with a report naming the offset and the word, like a PPU fault.
#pragma once

#include <array>
#include <atomic>
#include <optional>
#include <string>
#include <thread>

#include "lv2/kernel.h"
#include "rsx/rsx_backend.h"
#include "rsx/rsx_methods.h"

namespace cell::rsx {

inline constexpr u32 CELL_GCM_DISPLAY_FLIP_STATUS_DONE = 0;
inline constexpr u32 CELL_GCM_DISPLAY_FLIP_STATUS_WAITING = 1;

// RSX "interrupts" as seen by libgcm: called on the RSX thread, in command order. The receiver
// (hle::GcmInterrupts) only queues the event; the guest handler runs later on the guest
// interrupt PPU thread, never on this host thread.
class IRsxEventSink {
public:
    virtual ~IRsxEventSink() = default;
    virtual void OnFlipCommand(u32 buffer_id) = 0;  // a FIFO flip command was reached (queue handler)
    virtual void OnFlipDone(u32 buffer_id) = 0;     // display state updated, flip status DONE (flip handler)
    virtual void OnUserCommand(u32 cause) = 0;      // kGcmSetUserCommand reached (user handler)
};

// What the front end has seen, for the end / stall reports. Counters only: a decoded draw is
// not a rendered draw (the null back end rasterizes nothing).
struct RsxStats {
    u64 put_advances = 0;     // times the RSX thread found a new CellGcmControl.put (>= 1 flush each)
    u64 commands = 0;         // FIFO command words executed (method headers, jumps, calls, returns)
    u64 method_writes = 0;    // method arguments (register writes) processed
    u64 jumps = 0, calls = 0;
    u64 draw_begins = 0;      // NV4097_SET_BEGIN_END with a primitive (one draw batch each)
    u64 draw_ends = 0;        // NV4097_SET_BEGIN_END 0
    u64 draw_arrays = 0, draw_index_arrays = 0, inline_arrays = 0;
    u64 clears = 0;           // NV4097_CLEAR_SURFACE
    u64 fifo_flips = 0;       // flip commands reached in the FIFO
    u64 immediate_flips = 0;  // cellGcmSetFlipImmediate requests performed
    u64 user_commands = 0;
    u64 semaphore_acquires = 0, semaphore_releases = 0, references = 0;
    u64 frames_presented = 0; // from the back end (0 for the null back end)
    u32 put = 0, get = 0;     // current CellGcmControl values (0 before Start)
};

class RsxFifo {
public:
    struct Layout {
        u32 control_ea = 0;  // CellGcmControl {put, get, ref}
        u32 label_ea = 0;    // label (semaphore) area, kLabelCount * kLabelStride bytes
    };

    RsxFifo(lv2::Kernel& kernel, IRsxBackend& backend) noexcept : kernel_(kernel), backend_(backend) {
        for (auto& page : io_) page.store(kUnmapped, std::memory_order_relaxed);
    }
    ~RsxFifo() { Stop(); }
    RsxFifo(const RsxFifo&) = delete;
    RsxFifo& operator=(const RsxFifo&) = delete;

    void Start(const Layout& layout);
    void Stop();  // joins the thread; idempotent
    [[nodiscard]] bool running() const noexcept { return thread_.joinable(); }

    // ---- IO map (1 MiB pages); thread safe ----
    void MapIo(u32 io_offset, u32 ea, u32 size) noexcept;
    void UnmapIo(u32 io_offset, u32 size) noexcept;
    [[nodiscard]] std::optional<u32> IoToEa(u32 io_offset) const noexcept;

    // cellGcmSetFlipImmediate: flip to display buffer `id` outside the command stream. The
    // request is taken by the RSX thread at its next loop iteration (within ~1 ms when idle),
    // so the back end's OnFlip keeps running on the RSX thread only; a newer request replaces
    // one not yet taken.
    void RequestImmediateFlip(u32 id) noexcept { immediate_flip_.store(id + 1, std::memory_order_release); }
    [[nodiscard]] u64 immediate_flips() const noexcept { return immediate_flips_.load(std::memory_order_relaxed); }

    // Receiver of flip / queue / user events (set before Start; nullptr = none).
    void set_event_sink(IRsxEventSink* sink) noexcept { sink_.store(sink, std::memory_order_release); }
    [[nodiscard]] RsxStats Stats() const noexcept;

    // ---- display state the PPU queries (cellGcmGetFlipStatus...) ----
    std::atomic<u32> flip_status{CELL_GCM_DISPLAY_FLIP_STATUS_DONE};
    std::atomic<u32> current_buffer{0};
    std::atomic<u64> flip_count{0};
    std::atomic<u64> last_flip_time_us{0};
    [[nodiscard]] u64 commands() const noexcept { return commands_.load(std::memory_order_relaxed); }

private:
    static constexpr u32 kUnmapped = 0xFFFFFFFFu;

    void ThreadMain();
    // Executes the command at `get`; returns the next get, or nullopt after a fault / stop.
    std::optional<u32> Execute(u32 get);
    bool Method(u32 reg, u32 arg);
    bool WriteLabel(u32 offset, u32 value);
    void DoFlip(u32 buffer_id);  // RSX thread: present, then publish the display state
    void TakeImmediateFlip();
    void CountAction(u32 reg, u32 arg, u32 get) noexcept;  // draws / clears for the reports
    std::atomic<u32> immediate_flip_{0};  // pending buffer id + 1, 0 = none
    std::atomic<u64> immediate_flips_{0};
    std::atomic<IRsxEventSink*> sink_{nullptr};
    std::atomic<u32> published_control_{0};  // layout_.control_ea once Start() has run
    // Counters (written by the RSX thread only, read by reports: relaxed atomics).
    struct Counters {
        std::atomic<u64> put_advances{0}, method_writes{0}, jumps{0}, calls{0};
        std::atomic<u64> draw_begins{0}, draw_ends{0}, draw_arrays{0}, draw_index_arrays{0}, inline_arrays{0};
        std::atomic<u64> clears{0}, fifo_flips{0}, user_commands{0};
        std::atomic<u64> semaphore_acquires{0}, semaphore_releases{0}, references{0};
    } counters_;
    u32 current_get_ = 0;  // RSX thread: offset of the command being executed (first-event logs)
    [[nodiscard]] bool ShouldStop() const noexcept;
    void Fail(const char* what, u32 io_offset, u32 word);

    u32 Load(u32 ea) const noexcept;
    void Store(u32 ea, u32 value) noexcept;

    lv2::Kernel& kernel_;
    IRsxBackend& backend_;
    Layout layout_{};
    std::array<std::atomic<u32>, kIoPages> io_;  // io page -> EA of the page, or kUnmapped
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<u64> commands_{0};
    std::optional<u32> return_offset_;  // RSX call stack is one level deep
    u32 semaphore_offset_ = 0;          // NV406E_SEMAPHORE_OFFSET
    u32 back_end_semaphore_offset_ = 0; // NV4097_SET_SEMAPHORE_OFFSET
};

}  // namespace cell::rsx
