// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — rsx/rsx_fifo.cpp
#include "rsx/rsx_fifo.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "common/host_thread.h"
#include "common/log.h"

namespace cell::rsx {

namespace {
u64 NowMicroseconds() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}
}  // namespace

// ---------------------------------------------------------------------------------------
// IO map and guest access
// ---------------------------------------------------------------------------------------

void RsxFifo::MapIo(u32 io_offset, u32 ea, u32 size) noexcept {
    for (u32 i = 0; i < size / kIoPageSize; ++i) {
        const u32 page = io_offset / kIoPageSize + i;
        if (page < kIoPages) io_[page].store(ea + i * kIoPageSize, std::memory_order_release);
    }
}

void RsxFifo::UnmapIo(u32 io_offset, u32 size) noexcept {
    for (u32 i = 0; i < size / kIoPageSize; ++i) {
        const u32 page = io_offset / kIoPageSize + i;
        if (page < kIoPages) io_[page].store(kUnmapped, std::memory_order_release);
    }
}

std::optional<u32> RsxFifo::IoToEa(u32 io_offset) const noexcept {
    const u32 page = io_offset / kIoPageSize;
    if (page >= kIoPages) return std::nullopt;
    const u32 base = io_[page].load(std::memory_order_acquire);
    if (base == kUnmapped) return std::nullopt;
    return base + (io_offset % kIoPageSize);
}

u32 RsxFifo::Load(u32 ea) const noexcept {
    auto* p = reinterpret_cast<u32*>(kernel_.memory().shadow() + ea);
    return ByteSwap(std::atomic_ref<u32>(*p).load(std::memory_order_acquire));
}

void RsxFifo::Store(u32 ea, u32 value) noexcept {
    auto* p = reinterpret_cast<u32*>(kernel_.memory().shadow() + ea);
    std::atomic_ref<u32>(*p).store(ByteSwap(value), std::memory_order_release);
}

bool RsxFifo::ShouldStop() const noexcept {
    return stop_.load(std::memory_order_acquire) || kernel_.is_terminating();
}

void RsxFifo::Fail(const char* what, u32 io_offset, u32 word) {
    char text[192];
    std::snprintf(text, sizeof(text), "RSX FIFO: %s (get = IO offset 0x%08x, command word 0x%08x)", what, io_offset,
                  word);
    lv2::ProcessEnd end;
    end.kind = ppu::kExitAbort;
    end.message = text;
    LOG_ERROR("rsx", "%s", text);
    kernel_.RequestTermination(end);
}

// ---------------------------------------------------------------------------------------
// Thread
// ---------------------------------------------------------------------------------------

void RsxFifo::Start(const Layout& layout) {
    if (thread_.joinable()) return;
    layout_ = layout;
    published_control_.store(layout.control_ea, std::memory_order_release);  // for Stats() on other threads
    stop_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { ThreadMain(); });
}

void RsxFifo::Stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

void RsxFifo::DoFlip(u32 buffer_id) {
    backend_.OnFlip(buffer_id);
    current_buffer.store(buffer_id, std::memory_order_relaxed);
    last_flip_time_us.store(NowMicroseconds(), std::memory_order_relaxed);
    flip_count.fetch_add(1, std::memory_order_relaxed);
    flip_status.store(CELL_GCM_DISPLAY_FLIP_STATUS_DONE, std::memory_order_release);
    // Flip-done interrupt after the status is DONE (RPCS3 handle_emu_flip: flip_status = DONE,
    // then the flip handler is queued on the interrupt thread).
    if (IRsxEventSink* sink = sink_.load(std::memory_order_acquire)) sink->OnFlipDone(buffer_id);
}

void RsxFifo::TakeImmediateFlip() {
    if (const u32 pending = immediate_flip_.exchange(0, std::memory_order_acq_rel)) {
        immediate_flips_.fetch_add(1, std::memory_order_relaxed);
        DoFlip(pending - 1);
    }
}

RsxStats RsxFifo::Stats() const noexcept {
    const auto r = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
    RsxStats s;
    s.put_advances = r(counters_.put_advances);
    s.commands = r(commands_);
    s.method_writes = r(counters_.method_writes);
    s.jumps = r(counters_.jumps);
    s.calls = r(counters_.calls);
    s.draw_begins = r(counters_.draw_begins);
    s.draw_ends = r(counters_.draw_ends);
    s.draw_arrays = r(counters_.draw_arrays);
    s.draw_index_arrays = r(counters_.draw_index_arrays);
    s.inline_arrays = r(counters_.inline_arrays);
    s.clears = r(counters_.clears);
    s.fifo_flips = r(counters_.fifo_flips);
    s.immediate_flips = r(immediate_flips_);
    s.user_commands = r(counters_.user_commands);
    s.semaphore_acquires = r(counters_.semaphore_acquires);
    s.semaphore_releases = r(counters_.semaphore_releases);
    s.references = r(counters_.references);
    s.frames_presented = backend_.FramesPresented();
    if (const u32 control = published_control_.load(std::memory_order_acquire)) {
        s.put = Load(control + 0);
        s.get = Load(control + 4);
    }
    return s;
}

void RsxFifo::ThreadMain() {
    host::SetCurrentThreadName("RSX");
    log::tls_thread_name = "RSX";
    u32 idle_us = 20;
    u32 last_put = Load(layout_.control_ea + 0);
    while (!ShouldStop()) {
        TakeImmediateFlip();
        const u32 put = Load(layout_.control_ea + 0);
        u32 get = Load(layout_.control_ea + 4);
        if (put != last_put) {
            // A new put: at least one cellGcmFlush (several between two samples count once).
            if (counters_.put_advances.fetch_add(1, std::memory_order_relaxed) == 0)
                LOG_INFO("rsx", "first command-buffer submission: put 0x%08x (get 0x%08x)", put, get);
            last_put = put;
        }
        if (get == put) {
            // Nothing to do: back off up to 1 ms (put is a plain guest store, nobody notifies).
            host::SleepMicroseconds(idle_us);
            idle_us = std::min<u32>(idle_us * 2, 1000);
            continue;
        }
        idle_us = 20;
        while (get != put && !ShouldStop()) {
            current_get_ = get;
            const auto next = Execute(get);
            if (!next) {
                log::tls_thread_name = nullptr;
                return;
            }
            get = *next;
            Store(layout_.control_ea + 4, get);
            commands_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    log::tls_thread_name = nullptr;
}

std::optional<u32> RsxFifo::Execute(u32 get) {
    const auto ea = IoToEa(get);
    if (!ea || (get & 3)) {
        Fail("command buffer offset is not mapped in IO space", get, 0);
        return std::nullopt;
    }
    const u32 cmd = Load(*ea);

    if ((cmd & RSX_METHOD_OLD_JUMP_CMD_MASK) == RSX_METHOD_OLD_JUMP_CMD) {
        counters_.jumps.fetch_add(1, std::memory_order_relaxed);
        return cmd & RSX_METHOD_OLD_JUMP_OFFSET_MASK;
    }
    if ((cmd & RSX_METHOD_NEW_JUMP_CMD_MASK) == RSX_METHOD_NEW_JUMP_CMD) {
        counters_.jumps.fetch_add(1, std::memory_order_relaxed);
        return cmd & RSX_METHOD_OFFSET_MASK;
    }
    if ((cmd & RSX_METHOD_NEW_JUMP_CMD_MASK) == RSX_METHOD_CALL_CMD) {
        counters_.calls.fetch_add(1, std::memory_order_relaxed);
        if (return_offset_) {
            Fail("nested call (the RSX call stack is one level deep)", get, cmd);
            return std::nullopt;
        }
        return_offset_ = get + 4;
        return cmd & RSX_METHOD_OFFSET_MASK;
    }
    if ((cmd & RSX_METHOD_RETURN_MASK) == RSX_METHOD_RETURN_CMD) {
        if (!return_offset_) {
            Fail("return without call", get, cmd);
            return std::nullopt;
        }
        const u32 back = *return_offset_;
        return_offset_.reset();
        return back;
    }
    const u32 kind = cmd & RSX_METHOD_KIND_MASK;
    if (kind != RSX_METHOD_INCREMENT_CMD && kind != RSX_METHOD_NON_INCREMENT_CMD) {
        Fail("invalid command word", get, cmd);
        return std::nullopt;
    }
    const u32 count = (cmd >> RSX_METHOD_COUNT_SHIFT) & RSX_METHOD_COUNT_MASK;
    const u32 first = cmd & RSX_METHOD_REGISTER_MASK;
    for (u32 i = 0; i < count; ++i) {
        const u32 at = get + 4 + i * 4;
        const auto arg_ea = IoToEa(at);
        if (!arg_ea) {
            Fail("method argument is not mapped in IO space", at, cmd);
            return std::nullopt;
        }
        const u32 reg = kind == RSX_METHOD_NON_INCREMENT_CMD ? first : (first + i * 4) & RSX_METHOD_REGISTER_MASK;
        counters_.method_writes.fetch_add(1, std::memory_order_relaxed);
        if (!Method(reg, Load(*arg_ea))) return std::nullopt;
    }
    return get + 4 + count * 4;
}

bool RsxFifo::WriteLabel(u32 offset, u32 value) {
    if ((offset & 3) || offset >= kLabelCount * kLabelStride) {
        Fail("semaphore offset outside the label area", Load(layout_.control_ea + 4), offset);
        return false;
    }
    Store(layout_.label_ea + offset, value);
    return true;
}

// Draw / clear bookkeeping on the methods that go to the back end. The first draw and the
// first clear are logged once: they answer "does the game reach rendering at all?".
void RsxFifo::CountAction(u32 reg, u32 arg, u32 get) noexcept {
    switch (reg) {
    case NV4097_SET_BEGIN_END:
        if (arg) {
            if (counters_.draw_begins.fetch_add(1, std::memory_order_relaxed) == 0)
                LOG_INFO("rsx", "first draw: NV4097_SET_BEGIN_END primitive %u at IO offset 0x%08x (decoded only: the null back end "
                                "rasterizes nothing)", arg, get);
        } else {
            counters_.draw_ends.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    case NV4097_DRAW_ARRAYS: counters_.draw_arrays.fetch_add(1, std::memory_order_relaxed); break;
    case NV4097_DRAW_INDEX_ARRAY: counters_.draw_index_arrays.fetch_add(1, std::memory_order_relaxed); break;
    case NV4097_INLINE_ARRAY: counters_.inline_arrays.fetch_add(1, std::memory_order_relaxed); break;
    case NV4097_CLEAR_SURFACE:
        if (counters_.clears.fetch_add(1, std::memory_order_relaxed) == 0)
            LOG_INFO("rsx", "first clear: NV4097_CLEAR_SURFACE mask 0x%02x at IO offset 0x%08x (decoded only)", arg, get);
        break;
    default: break;
    }
}

bool RsxFifo::Method(u32 reg, u32 arg) {
    switch (reg) {
    case NV406E_SET_REFERENCE:
        counters_.references.fetch_add(1, std::memory_order_relaxed);
        Store(layout_.control_ea + 8, arg);
        return true;
    case NV406E_SET_CONTEXT_DMA_SEMAPHORE:
        return true;  // libgcm points both semaphore contexts at the label area
    case NV406E_SEMAPHORE_OFFSET:
        semaphore_offset_ = arg;
        return true;
    case NV406E_SEMAPHORE_ACQUIRE: {
        counters_.semaphore_acquires.fetch_add(1, std::memory_order_relaxed);
        if ((semaphore_offset_ & 3) || semaphore_offset_ >= kLabelCount * kLabelStride) {
            Fail("semaphore offset outside the label area", Load(layout_.control_ea + 4), semaphore_offset_);
            return false;
        }
        // Wait for the PPU (or an earlier release) to write the expected value.
        u32 sleep_us = 10;
        while (Load(layout_.label_ea + semaphore_offset_) != arg) {
            if (ShouldStop()) return false;
            host::SleepMicroseconds(sleep_us);
            sleep_us = std::min<u32>(sleep_us * 2, 500);
        }
        return true;
    }
    case NV406E_SEMAPHORE_RELEASE:
        counters_.semaphore_releases.fetch_add(1, std::memory_order_relaxed);
        return WriteLabel(semaphore_offset_, arg);
    case NV4097_SET_SEMAPHORE_OFFSET:
        back_end_semaphore_offset_ = arg;
        return true;
    case NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE:
        // The back end writes the value with its red/blue bytes swapped (as RPCS3 does).
        return WriteLabel(back_end_semaphore_offset_,
                          (arg & 0xFF00FF00u) | ((arg & 0xFF) << 16) | ((arg >> 16) & 0xFF));
    case NV4097_TEXTURE_READ_SEMAPHORE_RELEASE:
        return WriteLabel(back_end_semaphore_offset_, arg);
    case kHleFlipMethod:
        if (counters_.fifo_flips.fetch_add(1, std::memory_order_relaxed) == 0)
            LOG_INFO("rsx", "first FIFO flip: display buffer %u at IO offset 0x%08x (bookkeeping only: nothing is presented)", arg,
                     current_get_);
        // Queue interrupt first (RPCS3 flip_command: queue_handler, then the flip itself).
        if (IRsxEventSink* sink = sink_.load(std::memory_order_acquire)) sink->OnFlipCommand(arg);
        DoFlip(arg);
        return true;
    case kGcmSetUserCommand:
        counters_.user_commands.fetch_add(1, std::memory_order_relaxed);
        if (IRsxEventSink* sink = sink_.load(std::memory_order_acquire)) sink->OnUserCommand(arg);
        return true;
    default:
        CountAction(reg, arg, current_get_);
        backend_.OnMethod(reg, arg);
        return true;
    }
}

// ---------------------------------------------------------------------------------------
// Null back end report
// ---------------------------------------------------------------------------------------

std::vector<std::string> NullRsxBackend::TopMethods(std::size_t limit) const {
    std::vector<std::pair<u64, u32>> seen;
    for (u32 i = 0; i < counts_.size(); ++i)
        if (const u64 n = counts_[i].load(std::memory_order_relaxed)) seen.emplace_back(n, i << 2);
    std::sort(seen.begin(), seen.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    char line[64];
    for (std::size_t i = 0; i < seen.size() && i < limit; ++i) {
        std::snprintf(line, sizeof(line), "0x%04x x %llu", seen[i].second, static_cast<unsigned long long>(seen[i].first));
        out.emplace_back(line);
    }
    return out;
}

}  // namespace cell::rsx
