// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_gcm_sys.cpp
#include "hle/cell_gcm_sys.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <optional>
#include <string_view>
#include <thread>

#include "common/host_thread.h"
#include "common/log.h"
#include "hle/fnid.h"
#include "rsx/rsx_methods.h"

namespace cell::hle {

namespace {

using C = ppu::PpuThreadContext;
using M = vm::GuestMemory;

constexpr u32 kBlockSize = vm::k64K;
constexpr u32 kReservedCommandBytes = 0x1000;  // first 4 KiB of the io area stay unused
constexpr u32 kLocalReportsOffset = 0x0FE00000; // above the 249 MiB games may use
constexpr u32 kReportCount = 2048;
constexpr u32 kDisplayBuffers = 8, kTiles = 15, kZculls = 8;

// Guest words shared with the RSX thread: relaxed atomics, big-endian.
u32 GLoad(const M& m, u32 ea) {
    return ByteSwap(std::atomic_ref<u32>(*reinterpret_cast<u32*>(m.shadow() + ea)).load(std::memory_order_acquire));
}
void GStore(M& m, u32 ea, u32 value) {
    std::atomic_ref<u32>(*reinterpret_cast<u32*>(m.shadow() + ea)).store(ByteSwap(value), std::memory_order_release);
}

}  // namespace

CellGcmSys::CellGcmSys(lv2::Kernel& kernel)
    : kernel_(kernel), interrupts_(std::make_shared<GcmInterrupts>(kernel)), fifo_(kernel, backend_) {
    io_of_ea_.fill(0xFFFF);
    ea_of_io_.fill(0xFFFF);
    // In the object table: process termination wakes the interrupt thread's wait, and the
    // object outlives this one (the thread may still be inside a guest handler when we go).
    kernel_.objects().Add(kIdGcmInterrupts, interrupts_);
    fifo_.set_event_sink(interrupts_.get());
}

CellGcmSys::~CellGcmSys() {
    fifo_.Stop();          // no more flip / user events
    interrupts_->Close();  // the interrupt thread leaves its loop at its next wait
}

bool CellGcmSys::initialized() const { return block_.load(std::memory_order_acquire) != 0; }

CellGcmSys::Config CellGcmSys::config() const {
    std::lock_guard lock(mutex_);
    return config_;
}

void CellGcmSys::SetDefaultFifoSize(u32 buffer_size, u32 segment_size) {
    std::lock_guard lock(mutex_);
    if (segment_size >= 0x2000 && !(segment_size & 0xFFF)) segment_size_ = segment_size;
    LOG_INFO("cellGcmSys", "cellGcmSetDefaultFifoSize(buffer 0x%x, segment 0x%x)", buffer_size, segment_size);
}

// ---------------------------------------------------------------------------------------
// IO map: guest offset tables + host copies + the RSX thread's map
// ---------------------------------------------------------------------------------------

void CellGcmSys::MapIoLocked(u32 ea, u32 io, u32 size) {
    M& m = kernel_.memory();
    for (u32 i = 0; i < size / rsx::kIoPageSize; ++i) {
        const u32 ea_page = (ea >> 20) + i, io_page = (io >> 20) + i;
        io_of_ea_[ea_page] = static_cast<u16>(io_page);
        ea_of_io_[io_page] = static_cast<u16>(ea_page);
        m.Write<u16>(io_table_ea() + ea_page * 2, static_cast<u16>(io_page));
        m.Write<u16>(ea_table_ea() + io_page * 2, static_cast<u16>(ea_page));
    }
    io_blocks_[io] = size;
    fifo_.MapIo(io, ea, size);
}

void CellGcmSys::UnmapIoLocked(u32 io, u32 size) {
    M& m = kernel_.memory();
    fifo_.UnmapIo(io, size);
    for (u32 i = 0; i < size / rsx::kIoPageSize; ++i) {
        const u32 io_page = (io >> 20) + i;
        const u16 ea_page = ea_of_io_[io_page];
        if (ea_page != 0xFFFF && ea_page < io_of_ea_.size()) {
            io_of_ea_[ea_page] = 0xFFFF;
            m.Write<u16>(io_table_ea() + ea_page * 2, 0xFFFF);
        }
        ea_of_io_[io_page] = 0xFFFF;
        m.Write<u16>(ea_table_ea() + io_page * 2, 0xFFFF);
    }
    io_blocks_.erase(io);
}

s32 CellGcmSys::MapEaIoAddress(u32 ea, u32 io, u32 size) {
    if ((ea | io | size) & 0xFFFFF || !size) return CELL_GCM_ERROR_FAILURE;
    if (u64{io} + size > rsx::kIoSpaceSize || u64{ea} + size > 0xC0000000ull) return CELL_GCM_ERROR_FAILURE;
    if (!kernel_.memory().Check(ea, size, vm::kPageReadable)) return CELL_GCM_ERROR_FAILURE;
    std::lock_guard lock(mutex_);
    if (!block_.load(std::memory_order_relaxed)) return CELL_GCM_ERROR_FAILURE;
    for (u32 i = 0; i < size >> 20; ++i)
        if (ea_of_io_[(io >> 20) + i] != 0xFFFF || io_of_ea_[(ea >> 20) + i] != 0xFFFF) return CELL_GCM_ERROR_FAILURE;
    MapIoLocked(ea, io, size);
    return CELL_OK;
}

s32 CellGcmSys::MapMainMemory(u32 ea, u32 size, u32* io_offset) {
    if ((ea | size) & 0xFFFFF || !size || u64{ea} + size > 0xC0000000ull) return CELL_GCM_ERROR_FAILURE;
    if (!kernel_.memory().Check(ea, size, vm::kPageReadable)) {
        LOG_ERROR("cellGcmSys", "cellGcmMapMainMemory(0x%08x, 0x%x): range is not mapped", ea, size);
        return CELL_GCM_ERROR_FAILURE;
    }
    std::lock_guard lock(mutex_);
    if (!block_.load(std::memory_order_relaxed)) return CELL_GCM_ERROR_FAILURE;
    const u32 pages = size >> 20;
    for (u32 i = 0; i < pages; ++i)
        if (io_of_ea_[(ea >> 20) + i] != 0xFFFF) return CELL_GCM_ERROR_FAILURE;  // already mapped
    // First fit in IO space.
    for (u32 first = 0; first + pages <= rsx::kIoPages; ++first) {
        u32 n = 0;
        while (n < pages && ea_of_io_[first + n] == 0xFFFF) ++n;
        if (n == pages) {
            MapIoLocked(ea, first << 20, size);
            *io_offset = first << 20;
            LOG_INFO("cellGcmSys", "cellGcmMapMainMemory(0x%08x, 0x%x) -> IO offset 0x%08x", ea, size, *io_offset);
            return CELL_OK;
        }
        first += n;
    }
    return CELL_GCM_ERROR_NO_IO_PAGE_TABLE;
}

s32 CellGcmSys::UnmapIoAddress(u32 io) {
    std::lock_guard lock(mutex_);
    const auto it = io_blocks_.find(io);
    if (it == io_blocks_.end()) return CELL_GCM_ERROR_FAILURE;
    UnmapIoLocked(io, it->second);
    return CELL_OK;
}

s32 CellGcmSys::UnmapEaIoAddress(u32 ea) {
    u32 io = 0;
    {
        std::lock_guard lock(mutex_);
        if (ea >= 0xC0000000u || io_of_ea_[ea >> 20] == 0xFFFF) return CELL_GCM_ERROR_FAILURE;
        io = u32{io_of_ea_[ea >> 20]} << 20;
    }
    return UnmapIoAddress(io);
}

s32 CellGcmSys::AddressToOffset(u32 ea, u32* offset) const {
    if (ea >= rsx::kLocalMemoryBase && u64{ea} < u64{rsx::kLocalMemoryBase} + rsx::kLocalMemoryFull) {
        *offset = ea - rsx::kLocalMemoryBase;
        return CELL_OK;
    }
    std::lock_guard lock(mutex_);
    if (ea >= 0xC0000000u || io_of_ea_[ea >> 20] == 0xFFFF) return CELL_GCM_ERROR_FAILURE;
    *offset = (u32{io_of_ea_[ea >> 20]} << 20) | (ea & 0xFFFFF);
    return CELL_OK;
}

s32 CellGcmSys::IoOffsetToAddress(u32 io, u32* ea) const {
    std::lock_guard lock(mutex_);
    if ((io >> 20) >= ea_of_io_.size() || ea_of_io_[io >> 20] == 0xFFFF) return CELL_GCM_ERROR_FAILURE;
    *ea = (u32{ea_of_io_[io >> 20]} << 20) | (io & 0xFFFFF);
    return CELL_OK;
}

u32 CellGcmSys::ReportAddress(u32 index, u32 location) const {
    if (index >= kReportCount || !initialized()) return 0;
    if (location == CELL_GCM_LOCATION_LOCAL) return rsx::kLocalMemoryBase + kLocalReportsOffset + index * 16;
    if (location == CELL_GCM_LOCATION_MAIN) return At(0x8000) + index * 16;
    return 0;
}

// ---------------------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------------------

s32 CellGcmSys::InitBody(u32 context_ptr_addr, u32 cmd_size, u32 io_size, u32 io_address) {
    M& m = kernel_.memory();
    LOG_INFO("cellGcmSys", "_cellGcmInitBody(context ptr 0x%08x, cmd 0x%x, io 0x%x @ 0x%08x)", context_ptr_addr, cmd_size,
             io_size, io_address);
    const char* problem = nullptr;
    if (!m.Check(context_ptr_addr, 4, vm::kPageWritable)) problem = "context pointer is not writable";
    else if ((io_address | io_size) & 0xFFFFF || !io_size) problem = "io address/size not 1 MiB aligned";
    else if (io_size > rsx::kIoSpaceSize) problem = "io size larger than the IO space";
    else if (cmd_size <= kReservedCommandBytes + 0x100 || cmd_size > io_size) problem = "bad command buffer size";
    else if (!m.Check(io_address, io_size, vm::kPageWritable)) problem = "io area is not mapped read/write";
    if (problem) {
        LOG_ERROR("cellGcmSys", "_cellGcmInitBody: %s", problem);
        return CELL_GCM_ERROR_FAILURE;
    }

    std::lock_guard lock(mutex_);
    if (block_.load(std::memory_order_relaxed)) return CELL_GCM_ERROR_FAILURE;  // already initialized

    // RSX local memory (whole 256 MiB: the top holds the local reports).
    if (!m.AllocateFixed(vm::Area::RsxLocal, rsx::kLocalMemoryBase, rsx::kLocalMemoryFull, vm::kProtReadWrite)) {
        LOG_ERROR("cellGcmSys", "_cellGcmInitBody: cannot commit RSX local memory at 0x%08x", rsx::kLocalMemoryBase);
        return CELL_GCM_ERROR_FAILURE;
    }
    const auto block = m.Allocate(vm::Area::Main, kBlockSize, vm::k64K, vm::kProtReadWrite);
    if (!block) return CELL_GCM_ERROR_FAILURE;
    block_.store(*block, std::memory_order_release);
    for (u32 i = 0; i < io_of_ea_.size(); ++i) m.Write<u16>(io_table_ea() + i * 2, 0xFFFF);
    for (u32 i = 0; i < ea_of_io_.size(); ++i) m.Write<u16>(ea_table_ea() + i * 2, 0xFFFF);

    config_.local_address = rsx::kLocalMemoryBase;
    config_.local_size = rsx::kLocalMemoryGameSize;
    config_.io_address = io_address;
    config_.io_size = io_size;
    cmd_size_ = cmd_size;
    MapIoLocked(io_address, 0, io_size);  // the io area is IO offset 0

    // Default context: first segment of the command buffer.
    hle_->Materialize();
    const Segment first = SegmentAt(0);
    const u32 ctx = *block;
    GStore(m, ctx + 0, first.begin);
    GStore(m, ctx + 4, first.end);
    GStore(m, ctx + 8, first.begin);
    GStore(m, ctx + 12, hle_->OpdAddress(callback_index_));
    // Control registers: RSX idle at the start of the command buffer; ref = -1 (as after reset).
    GStore(m, control_ea() + 0, first.begin - io_address);
    GStore(m, control_ea() + 4, first.begin - io_address);
    GStore(m, control_ea() + 8, 0xFFFFFFFFu);
    context_ptr_ = context_ptr_addr;
    GStore(m, context_ptr_addr, ctx);

    fifo_.Start({control_ea(), label_ea()});
    // libgcm creates its interrupt thread here (RPCS3: "_gcm_intr_thread", prio 1, 0x4000).
    if (interrupts_enabled_) interrupts_->StartThread(hle_->CodeAddress(interrupt_wait_index_));
    LOG_INFO("cellGcmSys",
             "RSX ready: context 0x%08x (begin 0x%08x end 0x%08x), control 0x%08x, labels 0x%08x, local 0x%08x+0x%x, "
             "%u command segment(s) of 0x%x",
             ctx, first.begin, first.end, control_ea(), label_ea(), rsx::kLocalMemoryBase, rsx::kLocalMemoryGameSize,
             SegmentCount(), segment_size_);
    return CELL_OK;
}

// ---------------------------------------------------------------------------------------
// Command buffer segments, callback, flips
// ---------------------------------------------------------------------------------------

u32 CellGcmSys::SegmentCount() const { return (cmd_size_ + segment_size_ - 1) / segment_size_; }

CellGcmSys::Segment CellGcmSys::SegmentAt(u32 index) const {
    const u32 lo = std::max(index * segment_size_, kReservedCommandBytes);
    const u32 hi = std::min((index + 1) * segment_size_, cmd_size_);
    return {config_.io_address + lo, config_.io_address + hi - 4};
}

s32 CellGcmSys::Callback(u32 context_ea, u32 count) {
    M& m = kernel_.memory();
    u32 put = 0, begin_offset = 0, end_offset = 0;
    {
        std::lock_guard lock(mutex_);
        if (!block_.load(std::memory_order_relaxed) || !m.Check(context_ea, 16, vm::kPageWritable))
            return CELL_GCM_ERROR_FAILURE;
        const u32 current = GLoad(m, context_ea + 8);
        if (current < config_.io_address + kReservedCommandBytes || current > config_.io_address + cmd_size_ - 4) {
            LOG_ERROR("cellGcmSys", "cellGcmCallback: context 0x%08x current 0x%08x is not in the default command buffer",
                      context_ea, current);
            return CELL_GCM_ERROR_FAILURE;
        }
        const u32 index = (current - config_.io_address) / segment_size_;
        const u32 next_index = index + 1 < SegmentCount() ? index + 1 : 0;
        const Segment next = SegmentAt(next_index);
        if (count * 4 > next.end - next.begin) {
            LOG_ERROR("cellGcmSys", "cellGcmCallback: %u words do not fit a 0x%x-byte segment", count, segment_size_);
            return CELL_GCM_ERROR_FAILURE;
        }
        begin_offset = next.begin - config_.io_address;
        end_offset = next.end - config_.io_address;
        // Jump from here to the next segment; the RSX stops at the start of it (put).
        GStore(m, current, rsx::RSX_METHOD_OLD_JUMP_CMD | begin_offset);
        GStore(m, context_ea + 0, next.begin);
        GStore(m, context_ea + 4, next.end);
        GStore(m, context_ea + 8, next.begin);
        put = begin_offset;
    }
    GStore(m, control_ea() + 0, put);
    // The segment is reusable once the RSX has left it (or is parked at its first word).
    u32 sleep_us = 10;
    for (;;) {
        const u32 get = GLoad(m, control_ea() + 4);
        if (get == begin_offset || get < begin_offset || get > end_offset) break;
        if (kernel_.is_terminating()) break;
        host::SleepMicroseconds(sleep_us);
        sleep_us = std::min<u32>(sleep_us * 2, 500);
    }
    return CELL_OK;
}

s32 CellGcmSys::WriteFlip(u32 context_ea, u32 buffer_id, std::optional<std::pair<u32, u32>> label) {
    M& m = kernel_.memory();
    if (!m.Check(context_ea, 16, vm::kPageWritable)) return CELL_GCM_ERROR_FAILURE;
    const u32 words = label ? 6 : 2;
    u32 current = GLoad(m, context_ea + 8);
    if (current + words * 4 > GLoad(m, context_ea + 4)) {
        if (const s32 r = Callback(context_ea, words); r != CELL_OK) return r;
        current = GLoad(m, context_ea + 8);
    }
    if (!m.Check(current, words * 4, vm::kPageWritable)) return CELL_GCM_ERROR_FAILURE;
    GStore(m, current + 0, rsx::MethodHeader(rsx::kHleFlipMethod, 1));
    GStore(m, current + 4, buffer_id);
    if (label) {
        GStore(m, current + 8, rsx::MethodHeader(rsx::NV406E_SEMAPHORE_OFFSET, 1));
        GStore(m, current + 12, label->first * rsx::kLabelStride);
        GStore(m, current + 16, rsx::MethodHeader(rsx::NV406E_SEMAPHORE_RELEASE, 1));
        GStore(m, current + 20, label->second);
    }
    GStore(m, context_ea + 8, current + words * 4);
    flip_commands_written_.fetch_add(1, std::memory_order_relaxed);
    return CELL_OK;
}

namespace {
// Which handlers are delivered (hle/gcm_interrupts.h); the others are recorded only.
std::optional<GcmEvent> DeliveredKind(std::string_view which) {
    if (which == "cellGcmSetVBlankHandler") return GcmEvent::VBlank;
    if (which == "cellGcmSetFlipHandler") return GcmEvent::Flip;
    if (which == "cellGcmSetQueueHandler") return GcmEvent::Queue;
    if (which == "cellGcmSetUserHandler") return GcmEvent::User;
    return std::nullopt;
}
}  // namespace

void CellGcmSys::RecordHandler(const char* which, u32 opd) {
    bool first = false;
    {
        std::lock_guard lock(mutex_);
        first = handlers_.find(which) == handlers_.end();
        handlers_[which] = opd;
    }
    const auto kind = DeliveredKind(which);
    const u32 thread = interrupts_->thread_id();
    if (kind) interrupts_->SetHandler(*kind, opd);
    if (!kind) {
        if (first && opd)
            LOG_WARN("cellGcmSys", "%s(0x%08x): recorded, NOT delivered (semantics not verified; RPCS3 does not deliver it either)",
                     which, opd);
    } else if (!thread) {
        LOG_WARN("cellGcmSys", "%s(0x%08x): recorded, not delivered (no interrupt thread: %s)", which, opd,
                 interrupts_enabled_ ? "cellGcm not initialized" : "--no-gcm-interrupts");
    } else if (opd) {
        LOG_INFO("cellGcmSys", "%s(0x%08x): delivered on _gcm_intr_thread 0x%08x (%s)", which, opd, thread,
                 *kind == GcmEvent::VBlank ? "every vblank, 60 Hz, argument 1"
                 : *kind == GcmEvent::User ? "on each user command, argument = cause"
                                           : "on each event, argument 1");
    } else {
        LOG_INFO("cellGcmSys", "%s(0): handler disabled", which);
    }
}

u64 CellGcmSys::VBlankCount() const { return GcmInterrupts::VBlankIndex(GcmInterrupts::Now()); }

std::vector<std::string> CellGcmSys::Describe() const {
    std::vector<std::string> out;
    if (!initialized()) return out;
    using ull = unsigned long long;
    char line[256];
    const Config c = config();
    const rsx::RsxStats st = fifo_.Stats();
    std::snprintf(line, sizeof(line), "io 0x%08x+0x%x, local 0x%08x+0x%x; FIFO put 0x%08x get 0x%08x (%s)", c.io_address, c.io_size,
                  c.local_address, c.local_size, st.put, st.get, st.put == st.get ? "idle" : "pending commands");
    out.emplace_back(line);
    std::snprintf(line, sizeof(line),
                  "command buffer: %llu put advance(s) observed, %llu command word(s), %llu method write(s), %llu jump(s), %llu call(s)",
                  static_cast<ull>(st.put_advances), static_cast<ull>(st.commands), static_cast<ull>(st.method_writes),
                  static_cast<ull>(st.jumps), static_cast<ull>(st.calls));
    out.emplace_back(line);
    std::snprintf(line, sizeof(line),
                  "decoded: %llu draw begin(s) / %llu end(s) (NV4097_SET_BEGIN_END), %llu DRAW_ARRAYS, %llu DRAW_INDEX_ARRAY, "
                  "%llu INLINE_ARRAY, %llu CLEAR_SURFACE",
                  static_cast<ull>(st.draw_begins), static_cast<ull>(st.draw_ends), static_cast<ull>(st.draw_arrays),
                  static_cast<ull>(st.draw_index_arrays), static_cast<ull>(st.inline_arrays), static_cast<ull>(st.clears));
    out.emplace_back(line);
    std::snprintf(line, sizeof(line),
                  "sync: %llu reference(s), %llu semaphore acquire(s), %llu release(s), %llu user command(s)",
                  static_cast<ull>(st.references), static_cast<ull>(st.semaphore_acquires), static_cast<ull>(st.semaphore_releases),
                  static_cast<ull>(st.user_commands));
    out.emplace_back(line);
    std::snprintf(line, sizeof(line),
                  "flips: %llu flip command(s) written by cellGcmSetFlip*, %llu reached in the FIFO, %llu immediate; display buffer %u; "
                  "frames presented to a host surface: %llu (null back end: no rasterization, no presentation)",
                  static_cast<ull>(flip_commands_written()), static_cast<ull>(st.fifo_flips), static_cast<ull>(st.immediate_flips),
                  fifo_.current_buffer.load(), static_cast<ull>(st.frames_presented));
    out.emplace_back(line);
    if (const std::string irq = interrupts_->Describe(); !irq.empty()) out.push_back(irq);
    {
        std::lock_guard lock(mutex_);
        for (const auto& [name, opd] : handlers_)
            if (!DeliveredKind(name) && opd) out.push_back(name + " recorded, not delivered");
    }
    for (const std::string& m : backend_.TopMethods(12)) out.push_back("method " + m);
    return out;
}

// =======================================================================================
// Exports
// =======================================================================================

namespace gcm_exports {

CellGcmSys& Self(const HleFunction& f) { return *static_cast<CellGcmSys*>(f.user); }
u32 Arg32(const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); }
void Return(C& c, s64 value) { c.gpr[3] = static_cast<u64>(value); }

template <typename Fn>
void WithOut(C& c, M& m, u32 ptr, u32 bytes, Fn fn) {
    if (!m.Check(ptr, bytes, vm::kPageWritable)) return Return(c, CELL_GCM_ERROR_FAILURE);
    Return(c, fn());
}

// _cellGcmInitBody(CellGcmContextData** context, u32 cmdSize, u32 ioSize, u32 ioAddress)
void InitBody(C& c, M&, const HleFunction& f) {
    Return(c, Self(f).InitBody(Arg32(c, 0), Arg32(c, 1), Arg32(c, 2), Arg32(c, 3)));
}

// cellGcmMapMainMemory(void* ea, u32 size, u32* offset)
void MapMainMemory(C& c, M& m, const HleFunction& f) {
    WithOut(c, m, Arg32(c, 2), 4, [&] {
        u32 io = 0;
        const s32 r = Self(f).MapMainMemory(Arg32(c, 0), Arg32(c, 1), &io);
        if (r == CELL_OK) m.Write<u32>(Arg32(c, 2), io);
        return r;
    });
}
// cellGcmMapEaIoAddress(ea, io, size) / ...WithFlags(ea, io, size, flags)
void MapEaIoAddress(C& c, M&, const HleFunction& f) { Return(c, Self(f).MapEaIoAddress(Arg32(c, 0), Arg32(c, 1), Arg32(c, 2))); }
void UnmapEaIoAddress(C& c, M&, const HleFunction& f) { Return(c, Self(f).UnmapEaIoAddress(Arg32(c, 0))); }
void UnmapIoAddress(C& c, M&, const HleFunction& f) { Return(c, Self(f).UnmapIoAddress(Arg32(c, 0))); }

// cellGcmAddressToOffset(void* ea, u32* offset)
void AddressToOffset(C& c, M& m, const HleFunction& f) {
    WithOut(c, m, Arg32(c, 1), 4, [&] {
        u32 off = 0;
        const s32 r = Self(f).AddressToOffset(Arg32(c, 0), &off);
        if (r == CELL_OK) m.Write<u32>(Arg32(c, 1), off);
        return r;
    });
}
// cellGcmIoOffsetToAddress(u32 io, void** ea)
void IoOffsetToAddress(C& c, M& m, const HleFunction& f) {
    WithOut(c, m, Arg32(c, 1), 4, [&] {
        u32 ea = 0;
        const s32 r = Self(f).IoOffsetToAddress(Arg32(c, 0), &ea);
        if (r == CELL_OK) m.Write<u32>(Arg32(c, 1), ea);
        return r;
    });
}

// cellGcmGetConfiguration(CellGcmConfig* config): {localAddress, ioAddress, localSize, ioSize,
// memoryFrequency, coreFrequency}
void GetConfiguration(C& c, M& m, const HleFunction& f) {
    const u32 out = Arg32(c, 0);
    if (!m.Check(out, 24, vm::kPageWritable)) return Return(c, CELL_GCM_ERROR_FAILURE);
    const auto cfg = Self(f).config();
    m.Write<u32>(out + 0, cfg.local_address);
    m.Write<u32>(out + 4, cfg.io_address);
    m.Write<u32>(out + 8, cfg.local_size);
    m.Write<u32>(out + 12, cfg.io_size);
    m.Write<u32>(out + 16, cfg.memory_frequency);
    m.Write<u32>(out + 20, cfg.core_frequency);
    Return(c, CELL_OK);
}

// cellGcmGetOffsetTable(CellGcmOffsetTable* table): {u16* ioAddress, u16* eaAddress}
void GetOffsetTable(C& c, M& m, const HleFunction& f) {
    const u32 out = Arg32(c, 0);
    if (!m.Check(out, 8, vm::kPageWritable)) return Return(c, CELL_GCM_ERROR_FAILURE);
    m.Write<u32>(out + 0, Self(f).io_table_ea());
    m.Write<u32>(out + 4, Self(f).ea_table_ea());
    Return(c, CELL_OK);
}

void GetMaxIoMapSize(C& c, M&, const HleFunction& f) {
    Return(c, rsx::kIoSpaceSize - Self(f).config().io_size);
}

// cellGcmMapLocalMemory(void** address, u32* size)
void MapLocalMemory(C& c, M& m, const HleFunction& f) {
    if (!m.Check(Arg32(c, 0), 4, vm::kPageWritable) || !m.Check(Arg32(c, 1), 4, vm::kPageWritable))
        return Return(c, CELL_GCM_ERROR_FAILURE);
    const auto cfg = Self(f).config();
    if (!cfg.local_address) return Return(c, CELL_GCM_ERROR_FAILURE);
    m.Write<u32>(Arg32(c, 0), cfg.local_address);
    m.Write<u32>(Arg32(c, 1), cfg.local_size);
    Return(c, CELL_OK);
}

void GetControlRegister(C& c, M&, const HleFunction& f) { Return(c, Self(f).control_ea()); }
void GetLabelAddress(C& c, M&, const HleFunction& f) {
    const u32 index = Arg32(c, 0) & 0xFF;
    Return(c, Self(f).initialized() ? Self(f).label_ea() + index * rsx::kLabelStride : 0);
}
// cellGcmGetReportDataAddressLocation(u32 index, u32 location) / cellGcmGetReportDataAddress(u32 index)
void GetReportDataAddressLocation(C& c, M&, const HleFunction& f) { Return(c, Self(f).ReportAddress(Arg32(c, 0), Arg32(c, 1))); }
void GetReportDataAddress(C& c, M&, const HleFunction& f) {
    Return(c, Self(f).ReportAddress(Arg32(c, 0), CELL_GCM_LOCATION_LOCAL));
}
// CellGcmReportData {u64 timer; u32 value; u32 zero}
void GetReportDataLocation(C& c, M& m, const HleFunction& f) {
    const u32 at = Self(f).ReportAddress(Arg32(c, 0), Arg32(c, 1));
    Return(c, at ? m.Read<u32>(at + 8) : 0);
}
void GetTimeStampLocation(C& c, M& m, const HleFunction& f) {
    const u32 at = Self(f).ReportAddress(Arg32(c, 0), Arg32(c, 1));
    c.gpr[3] = at ? m.Read<u64>(at) : 0;
}
void GetTimeStamp(C& c, M& m, const HleFunction& f) {
    const u32 at = Self(f).ReportAddress(Arg32(c, 0), CELL_GCM_LOCATION_LOCAL);
    c.gpr[3] = at ? m.Read<u64>(at) : 0;
}

// Display
// cellGcmSetDisplayBuffer(u8 id, u32 offset, u32 pitch, u32 width, u32 height)
void SetDisplayBuffer(C& c, M& m, const HleFunction& f) {
    const u32 id = Arg32(c, 0) & 0xFF;
    if (id >= kDisplayBuffers || !Self(f).initialized()) return Return(c, CELL_GCM_ERROR_FAILURE);
    const u32 at = Self(f).display_info_ea() + id * 16;
    for (int i = 0; i < 4; ++i) m.Write<u32>(at + 4 * i, Arg32(c, 1 + i));
    LOG_INFO("cellGcmSys", "cellGcmSetDisplayBuffer(%u, offset 0x%08x, pitch %u, %ux%u)", id, Arg32(c, 1), Arg32(c, 2),
             Arg32(c, 3), Arg32(c, 4));
    Return(c, CELL_OK);
}
void GetDisplayInfo(C& c, M&, const HleFunction& f) { Return(c, Self(f).display_info_ea()); }
void GetTileInfo(C& c, M&, const HleFunction& f) { Return(c, Self(f).tile_info_ea()); }
void GetZcullInfo(C& c, M&, const HleFunction& f) { Return(c, Self(f).zcull_info_ea()); }
void GetCurrentDisplayBufferId(C& c, M& m, const HleFunction& f) {
    if (!m.Check(Arg32(c, 0), 1, vm::kPageWritable)) return Return(c, CELL_GCM_ERROR_FAILURE);
    m.Write<u8>(Arg32(c, 0), static_cast<u8>(Self(f).fifo().current_buffer.load()));
    Return(c, CELL_OK);
}
void GetFlipStatus(C& c, M&, const HleFunction& f) { Return(c, Self(f).fifo().flip_status.load()); }
void ResetFlipStatus(C& c, M&, const HleFunction& f) {
    Self(f).fifo().flip_status.store(rsx::CELL_GCM_DISPLAY_FLIP_STATUS_WAITING);
    Return(c, CELL_OK);
}
void SetFlipStatus(C& c, M&, const HleFunction& f) {
    Self(f).fifo().flip_status.store(rsx::CELL_GCM_DISPLAY_FLIP_STATUS_DONE);
    Return(c, CELL_OK);
}
void GetLastFlipTime(C& c, M&, const HleFunction& f) { c.gpr[3] = Self(f).fifo().last_flip_time_us.load(); }
void GetVBlankCount(C& c, M&, const HleFunction& f) { c.gpr[3] = Self(f).VBlankCount(); }

// Flip commands written into the caller's command buffer.
// _cellGcmSetFlipCommand / cellGcmSetFlipCommand / cellGcmSetFlip (CellGcmContextData*, u32 id)
void SetFlipCommand(C& c, M&, const HleFunction& f) { Return(c, Self(f).WriteFlip(Arg32(c, 0), Arg32(c, 1), std::nullopt)); }
// cellGcmSetPrepareFlip(CellGcmContextData*, u32 id) -> id
void SetPrepareFlip(C& c, M&, const HleFunction& f) {
    const s32 r = Self(f).WriteFlip(Arg32(c, 0), Arg32(c, 1), std::nullopt);
    Return(c, r == CELL_OK ? static_cast<s64>(Arg32(c, 1)) : r);
}
// _cellGcmSetFlipCommandWithWaitLabel(CellGcmContextData*, u32 id, u32 label_index, u32 label_value)
void SetFlipCommandWithWaitLabel(C& c, M&, const HleFunction& f) {
    const u32 index = Arg32(c, 2);
    if (index >= rsx::kLabelCount) return Return(c, CELL_GCM_ERROR_INVALID_VALUE);
    Return(c, Self(f).WriteFlip(Arg32(c, 0), Arg32(c, 1), std::make_pair(index, Arg32(c, 3))));
}
// cellGcmSetWaitFlip(CellGcmContextData*): flips are processed in FIFO order, so the commands
// that follow already run after the flip. Nothing to emit.
void SetWaitFlip(C& c, M&, const HleFunction&) { Return(c, CELL_OK); }

// cellGcmSetFlipImmediate(u8 id) — NID 0xACEE8542 (SHA-1 FNID of the name). Flips to display
// buffer `id` without a command in the command buffer (no wait for the RSX to reach a flip
// method). Same display-state effect as a FIFO flip; performed by the RSX thread. id > 7 or GCM
// not initialized: CELL_GCM_ERROR_FAILURE.
void SetFlipImmediate(C& c, M&, const HleFunction& f) {
    const u32 id = Arg32(c, 0) & 0xFF;
    CellGcmSys& s = Self(f);
    if (id >= kDisplayBuffers || !s.initialized()) return Return(c, CELL_GCM_ERROR_FAILURE);
    s.fifo().RequestImmediateFlip(id);
    LOG_TRACE("cellGcmSys", "cellGcmSetFlipImmediate(%u)", id);
    Return(c, CELL_OK);
}

// cellGcmCallback(CellGcmContextData*, u32 count): the context's callback (OPD set by init)
void Callback(C& c, M&, const HleFunction& f) { Return(c, Self(f).Callback(Arg32(c, 0), Arg32(c, 1))); }

// Runtime-internal: the HLE call of _gcm_intr_thread's trampoline. Blocks until a handler is
// due; r3 = its OPD (0 = leave the loop), r4 = its argument.
void InterruptWait(C& c, M&, const HleFunction& f) {
    const auto [opd, arg] = static_cast<GcmInterrupts*>(f.user)->WaitNext();
    c.gpr[3] = opd;
    c.gpr[4] = arg;
}

void SetDefaultCommandBuffer(C& c, M& m, const HleFunction& f) {
    CellGcmSys& s = Self(f);
    if (s.initialized() && s.context_ptr_addr()) m.Write<u32>(s.context_ptr_addr(), s.context_ea());
    Return(c, CELL_OK);
}
// cellGcmSetDefaultFifoSize(u32 bufferSize, u32 segmentSize)
void SetDefaultFifoSize(C& c, M&, const HleFunction& f) {
    Self(f).SetDefaultFifoSize(Arg32(c, 0), Arg32(c, 1));
    Return(c, CELL_OK);
}
void GetDefaultCommandWordSize(C& c, M&, const HleFunction&) { Return(c, 0x400); }
void GetDefaultSegmentWordSize(C& c, M&, const HleFunction&) { Return(c, 0x100); }

// cellGcmGetTiledPitchSize(u32 size): smallest tile-able pitch >= size (0 if too large)
void GetTiledPitchSize(C& c, M&, const HleFunction&) {
    static constexpr u32 kPitches[] = {0x0000, 0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800, 0x0A00,
                                       0x0C00, 0x0D00, 0x0E00, 0x1000, 0x1400, 0x1800, 0x1A00, 0x1C00, 0x2000,
                                       0x2800, 0x3000, 0x3400, 0x3800, 0x4000, 0x5000, 0x6000, 0x6800, 0x7000,
                                       0x8000, 0xA000, 0xC000, 0xD000, 0xE000, 0x10000};
    const u32 size = Arg32(c, 0);
    for (std::size_t i = 0; i + 1 < std::size(kPitches); ++i)
        if (kPitches[i] < size && size <= kPitches[i + 1]) return Return(c, kPitches[i + 1]);
    Return(c, 0);
}

// Tiles / zcull: validated and accepted; the renderer will consume them later.
// cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size, u32 pitch, u8 comp, u16 base, u8 bank)
void SetTileInfo(C& c, M&, const HleFunction&) {
    if ((Arg32(c, 0) & 0xFF) >= kTiles) return Return(c, CELL_GCM_ERROR_INVALID_VALUE);
    if ((Arg32(c, 1) & 0xFF) > CELL_GCM_LOCATION_MAIN) return Return(c, CELL_GCM_ERROR_INVALID_ENUM);
    if ((Arg32(c, 2) & 0xFFFF) || (Arg32(c, 3) & 0xFFFF) || (Arg32(c, 4) & 0xFF))
        return Return(c, CELL_GCM_ERROR_INVALID_ALIGNMENT);
    Return(c, CELL_OK);
}
void BindTile(C& c, M&, const HleFunction&) {
    Return(c, (Arg32(c, 0) & 0xFF) >= kTiles ? CELL_GCM_ERROR_INVALID_VALUE : CELL_OK);
}
void BindZcull(C& c, M&, const HleFunction&) {
    Return(c, (Arg32(c, 0) & 0xFF) >= kZculls ? CELL_GCM_ERROR_INVALID_VALUE : CELL_OK);
}

void Accept(C& c, M&, const HleFunction& f) {
    LOG_TRACE("cellGcmSys", "%s: accepted", f.name.c_str());
    Return(c, CELL_OK);
}
void ReturnZero(C& c, M&, const HleFunction&) { Return(c, 0); }

template <const char* Name>
void Handler(C& c, M&, const HleFunction& f) {
    Self(f).RecordHandler(Name, Arg32(c, 0));
    Return(c, CELL_OK);
}
constexpr char kFlipHandler[] = "cellGcmSetFlipHandler";
constexpr char kVBlankHandler[] = "cellGcmSetVBlankHandler";
constexpr char kSecondVHandler[] = "cellGcmSetSecondVHandler";
constexpr char kUserHandler[] = "cellGcmSetUserHandler";
constexpr char kGraphicsHandler[] = "cellGcmSetGraphicsHandler";
constexpr char kQueueHandler[] = "cellGcmSetQueueHandler";

}  // namespace gcm_exports

void CellGcmSys::Register(HleRegistry& r) {
    hle_ = &r;
    constexpr const char* kModule = "cellGcmSys";
    struct Entry {
        const char* name;
        HleFunc fn;
    };
    static constexpr Entry kEntries[] = {
        {"_cellGcmInitBody", gcm_exports::InitBody},
        {"cellGcmMapMainMemory", gcm_exports::MapMainMemory},
        {"cellGcmMapEaIoAddress", gcm_exports::MapEaIoAddress},
        {"cellGcmMapEaIoAddressWithFlags", gcm_exports::MapEaIoAddress},
        {"cellGcmUnmapEaIoAddress", gcm_exports::UnmapEaIoAddress},
        {"cellGcmUnmapIoAddress", gcm_exports::UnmapIoAddress},
        {"cellGcmAddressToOffset", gcm_exports::AddressToOffset},
        {"cellGcmIoOffsetToAddress", gcm_exports::IoOffsetToAddress},
        {"cellGcmGetConfiguration", gcm_exports::GetConfiguration},
        {"cellGcmGetOffsetTable", gcm_exports::GetOffsetTable},
        {"cellGcmGetMaxIoMapSize", gcm_exports::GetMaxIoMapSize},
        {"cellGcmMapLocalMemory", gcm_exports::MapLocalMemory},
        {"cellGcmGetControlRegister", gcm_exports::GetControlRegister},
        {"cellGcmGetLabelAddress", gcm_exports::GetLabelAddress},
        {"cellGcmGetReportDataAddressLocation", gcm_exports::GetReportDataAddressLocation},
        {"cellGcmGetReportDataAddress", gcm_exports::GetReportDataAddress},
        {"cellGcmGetReportDataLocation", gcm_exports::GetReportDataLocation},
        {"cellGcmGetTimeStampLocation", gcm_exports::GetTimeStampLocation},
        {"cellGcmGetTimeStamp", gcm_exports::GetTimeStamp},
        {"cellGcmSetDisplayBuffer", gcm_exports::SetDisplayBuffer},
        {"cellGcmGetDisplayInfo", gcm_exports::GetDisplayInfo},
        {"cellGcmGetTileInfo", gcm_exports::GetTileInfo},
        {"cellGcmGetZcullInfo", gcm_exports::GetZcullInfo},
        {"cellGcmGetCurrentDisplayBufferId", gcm_exports::GetCurrentDisplayBufferId},
        {"cellGcmGetFlipStatus", gcm_exports::GetFlipStatus},
        {"cellGcmResetFlipStatus", gcm_exports::ResetFlipStatus},
        {"cellGcmSetFlipStatus", gcm_exports::SetFlipStatus},
        {"cellGcmGetLastFlipTime", gcm_exports::GetLastFlipTime},
        {"cellGcmGetVBlankCount", gcm_exports::GetVBlankCount},
        {"cellGcmSetFlipCommand", gcm_exports::SetFlipCommand},
        {"_cellGcmSetFlipCommand", gcm_exports::SetFlipCommand},
        {"cellGcmSetFlip", gcm_exports::SetFlipCommand},
        {"cellGcmSetPrepareFlip", gcm_exports::SetPrepareFlip},
        {"_cellGcmSetFlipCommandWithWaitLabel", gcm_exports::SetFlipCommandWithWaitLabel},
        {"cellGcmSetFlipCommandWithWaitLabel", gcm_exports::SetFlipCommandWithWaitLabel},
        {"cellGcmSetFlipImmediate", gcm_exports::SetFlipImmediate},
        {"cellGcmSetWaitFlip", gcm_exports::SetWaitFlip},
        {"cellGcmSetDefaultCommandBuffer", gcm_exports::SetDefaultCommandBuffer},
        {"cellGcmSetDefaultFifoSize", gcm_exports::SetDefaultFifoSize},
        {"cellGcmGetDefaultCommandWordSize", gcm_exports::GetDefaultCommandWordSize},
        {"cellGcmGetDefaultSegmentWordSize", gcm_exports::GetDefaultSegmentWordSize},
        {"cellGcmInitDefaultFifoMode", gcm_exports::Accept},
        {"cellGcmGetTiledPitchSize", gcm_exports::GetTiledPitchSize},
        {"cellGcmSetTileInfo", gcm_exports::SetTileInfo},
        {"cellGcmSetTile", gcm_exports::SetTileInfo},
        {"cellGcmBindTile", gcm_exports::BindTile},
        {"cellGcmUnbindTile", gcm_exports::BindTile},
        {"cellGcmSetZcull", gcm_exports::BindZcull},
        {"cellGcmBindZcull", gcm_exports::BindZcull},
        {"cellGcmUnbindZcull", gcm_exports::BindZcull},
        {"cellGcmSetFlipMode", gcm_exports::Accept},
        {"cellGcmSetDebugOutputLevel", gcm_exports::Accept},
        {"cellGcmSetVBlankFrequency", gcm_exports::Accept},
        {"cellGcmSetSecondVFrequency", gcm_exports::Accept},
        {"cellGcmSortRemapEaIoAddress", gcm_exports::Accept},
        {"cellGcmGetCurrentField", gcm_exports::ReturnZero},
        {"cellGcmGetLastSecondVTime", gcm_exports::ReturnZero},
        {"_cellGcmFunc15", gcm_exports::ReturnZero},
        {"cellGcmSetFlipHandler", gcm_exports::Handler<gcm_exports::kFlipHandler>},
        {"cellGcmSetVBlankHandler", gcm_exports::Handler<gcm_exports::kVBlankHandler>},
        {"cellGcmSetSecondVHandler", gcm_exports::Handler<gcm_exports::kSecondVHandler>},
        {"cellGcmSetUserHandler", gcm_exports::Handler<gcm_exports::kUserHandler>},
        {"cellGcmSetGraphicsHandler", gcm_exports::Handler<gcm_exports::kGraphicsHandler>},
        {"cellGcmSetQueueHandler", gcm_exports::Handler<gcm_exports::kQueueHandler>},
    };
    for (const Entry& e : kEntries) r.Register(kModule, e.name, e.fn, this);
    // Not an export: the callback the default context points at. Bound now so the loader's
    // Materialize() places its thunk/OPD together with the imports.
    r.Register(kModule, "cellGcmCallback", gcm_exports::Callback, this);
    callback_index_ = r.Bind(kModule, ComputeFnid("cellGcmCallback"));
    // Not importable by games ("$internal" module): the interrupt thread's wait.
    r.Register("$internal", "gcm_interrupt_wait", gcm_exports::InterruptWait, interrupts_.get());
    interrupt_wait_index_ = r.Bind("$internal", ComputeFnid("gcm_interrupt_wait"));
}

}  // namespace cell::hle
