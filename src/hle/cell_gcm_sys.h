// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_gcm_sys.h
//
// cellGcmSys: the system half of libgcm (the static libgcm_cmd inlines that *write* commands
// are compiled into the game). On hardware it drives the RSX through lv2 (sys_rsx_* syscalls);
// here it is HLE on top of rsx::RsxFifo, the RSX front-end thread.
//
// cellGcmInit(cmdSize, ioSize, ioAddress) is a libgcm_cmd inline that calls
//   _cellGcmInitBody(&gCellGcmCurrentContext, cmdSize, ioSize, ioAddress)
// and every command-writing inline then reads gCellGcmCurrentContext->current. If
// _cellGcmInitBody does not store a valid CellGcmContextData* there, the first inline after
// init dereferences null — the MGS4 crash at 0x002ed878 (`lwz r9,0(r11)` loads the context
// pointer, `lwz r3,8(r9)` reads context->current).
//
// Guest structures (big-endian, in one 64 KiB block of the main area):
//   +0x0000 CellGcmContextData { begin, end, current, callback (OPD) }
//   +0x0040 CellGcmControl     { put, get, ref }              cellGcmGetControlRegister
//   +0x1000 labels             256 x 16 bytes                 cellGcmGetLabelAddress
//   +0x2000 CellGcmDisplayInfo 8 x { offset, pitch, width, height }
//   +0x2100 CellGcmTileInfo    15 x 16 bytes (zeroed; register packing in a later phase)
//   +0x2200 CellGcmZcullInfo   8 x 32 bytes (same)
//   +0x3000 offset table ioAddress[3072] (u16: EA>>20 -> IO>>20, 0xFFFF = unmapped)
//   +0x5000 offset table eaAddress[512]  (u16: IO>>20 -> EA>>20)
//   +0x8000 main-memory reports 2048 x 16 bytes
// RSX local memory: 256 MiB committed at 0xC0000000; games are told 0x0F900000 bytes, the
// local reports live above that at local offset 0x0FE00000.
//
// Command buffer: IO offsets [0, cmdSize) of the io area, first 4 KiB reserved, split into
// segments (32 KiB unless cellGcmSetDefaultFifoSize said otherwise). When a segment is full the
// game calls context->callback (our cellGcmCallback): it writes a jump to the next segment,
// flushes, and waits until the RSX has left that segment.
//
// Interrupt handlers (phase 7.7): _cellGcmInitBody also creates the guest interrupt thread
// "_gcm_intr_thread" (as libgcm does) and the vblank / flip / queue / user handlers are
// delivered on it (hle/gcm_interrupts.h). The second-V and graphics handlers are recorded and
// reported, not delivered (RPCS3 ignores them too; their semantics are not verified).
#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hle/gcm_interrupts.h"
#include "hle/hle_registry.h"
#include "lv2/kernel.h"
#include "rsx/rsx_backend.h"
#include "rsx/rsx_fifo.h"

namespace cell::hle {

inline constexpr s32 CELL_GCM_ERROR_FAILURE = static_cast<s32>(0x802100FFu);
inline constexpr s32 CELL_GCM_ERROR_NO_IO_PAGE_TABLE = static_cast<s32>(0x80210001u);
inline constexpr s32 CELL_GCM_ERROR_INVALID_ENUM = static_cast<s32>(0x80210002u);
inline constexpr s32 CELL_GCM_ERROR_INVALID_VALUE = static_cast<s32>(0x80210003u);
inline constexpr s32 CELL_GCM_ERROR_INVALID_ALIGNMENT = static_cast<s32>(0x80210004u);
inline constexpr s32 CELL_GCM_ERROR_ADDRESS_OVERLAP = static_cast<s32>(0x80210005u);

inline constexpr u32 CELL_GCM_LOCATION_LOCAL = 0;
inline constexpr u32 CELL_GCM_LOCATION_MAIN = 1;

class CellGcmSys {
public:
    explicit CellGcmSys(lv2::Kernel& kernel);
    ~CellGcmSys();
    CellGcmSys(const CellGcmSys&) = delete;
    CellGcmSys& operator=(const CellGcmSys&) = delete;

    void Register(HleRegistry& registry);

    // ---- operations behind the exports (also used by tests); return CELL_OK / CELL_GCM_ERROR_* ----
    s32 InitBody(u32 context_ptr_addr, u32 cmd_size, u32 io_size, u32 io_address);
    s32 MapMainMemory(u32 ea, u32 size, u32* io_offset);
    s32 MapEaIoAddress(u32 ea, u32 io, u32 size);
    s32 UnmapEaIoAddress(u32 ea);
    s32 UnmapIoAddress(u32 io);
    s32 AddressToOffset(u32 ea, u32* offset) const;
    s32 IoOffsetToAddress(u32 io, u32* ea) const;
    s32 Callback(u32 context_ea, u32 count);
    // Appends a flip (and optionally a label release) to the context's command buffer.
    s32 WriteFlip(u32 context_ea, u32 buffer_id, std::optional<std::pair<u32, u32>> label);
    void SetDefaultFifoSize(u32 buffer_size, u32 segment_size);

    [[nodiscard]] bool initialized() const;
    [[nodiscard]] u32 context_ea() const noexcept { return block_.load(std::memory_order_acquire); }
    [[nodiscard]] u32 control_ea() const noexcept { return At(0x40); }
    [[nodiscard]] u32 label_ea() const noexcept { return At(0x1000); }
    [[nodiscard]] u32 display_info_ea() const noexcept { return At(0x2000); }
    [[nodiscard]] u32 tile_info_ea() const noexcept { return At(0x2100); }
    [[nodiscard]] u32 zcull_info_ea() const noexcept { return At(0x2200); }
    [[nodiscard]] u32 io_table_ea() const noexcept { return At(0x3000); }
    [[nodiscard]] u32 ea_table_ea() const noexcept { return At(0x5000); }
    [[nodiscard]] u32 ReportAddress(u32 index, u32 location) const;
    [[nodiscard]] u32 context_ptr_addr() const noexcept { return context_ptr_; }

    rsx::RsxFifo& fifo() noexcept { return fifo_; }
    rsx::NullRsxBackend& backend() noexcept { return backend_; }
    lv2::Kernel& kernel() noexcept { return kernel_; }

    // Display / handler bookkeeping used by the exports.
    struct Config {
        u32 local_address = 0, io_address = 0, local_size = 0, io_size = 0;
        u32 memory_frequency = 650000000, core_frequency = 500000000;
    };
    [[nodiscard]] Config config() const;
    void RecordHandler(const char* which, u32 opd);
    [[nodiscard]] u64 VBlankCount() const;
    // false: no interrupt thread, handlers only recorded (the phase 7.6 behaviour; diagnostic
    // A/B switch --no-gcm-interrupts). Set before _cellGcmInitBody.
    void set_interrupts_enabled(bool on) noexcept { interrupts_enabled_ = on; }
    [[nodiscard]] GcmInterrupts& interrupts() noexcept { return *interrupts_; }
    [[nodiscard]] u64 flip_commands_written() const noexcept { return flip_commands_written_.load(std::memory_order_relaxed); }

    // End-of-process report lines (empty when cellGcm was never initialized).
    [[nodiscard]] std::vector<std::string> Describe() const;

private:
    [[nodiscard]] u32 At(u32 offset) const noexcept {
        const u32 b = block_.load(std::memory_order_acquire);
        return b ? b + offset : 0;
    }
    struct Segment {
        u32 begin, end;  // EAs; `end` keeps 4 bytes for the jump
    };
    [[nodiscard]] Segment SegmentAt(u32 index) const;  // mutex_ held
    [[nodiscard]] u32 SegmentCount() const;            // mutex_ held
    void MapIoLocked(u32 ea, u32 io, u32 size);
    void UnmapIoLocked(u32 io, u32 size);

    lv2::Kernel& kernel_;
    HleRegistry* hle_ = nullptr;
    u32 callback_index_ = 0;
    u32 interrupt_wait_index_ = 0;
    bool interrupts_enabled_ = true;
    std::shared_ptr<GcmInterrupts> interrupts_;  // also held by the kernel object table
    std::atomic<u64> flip_commands_written_{0};  // flip commands HLE wrote into command buffers
    rsx::NullRsxBackend backend_;
    rsx::RsxFifo fifo_;

    mutable std::mutex mutex_;
    std::atomic<u32> block_{0};  // guest block with the structures above (0 until init; set once)
    u32 context_ptr_ = 0;  // where the game keeps its CellGcmContextData* (gCellGcmCurrentContext)
    Config config_;
    u32 cmd_size_ = 0;
    u32 segment_size_ = 0x8000;
    std::array<u16, 3072> io_of_ea_{};  // host copies of the offset tables
    std::array<u16, 512> ea_of_io_{};
    std::map<u32, u32> io_blocks_;      // io offset -> size, for unmap
    std::map<std::string, u32> handlers_;
};

}  // namespace cell::hle
