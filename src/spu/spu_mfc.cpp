// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — spu/spu_mfc.cpp
#include "spu/spu_mfc.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>
#include <cstring>

#include "common/log.h"
#include "lv2/sys_mmapper.h"
#include "memory/guest_copy.h"
#include "ppu/ppu_code_cache.h"

namespace cell::spu {

namespace {

// Striped reservation locks: one 128-byte line maps to one stripe.
std::array<std::mutex, 256> g_stripes;
std::mutex& Stripe(u32 ea) { return g_stripes[(ea >> 7) & 255]; }

constexpr u32 kSnr1Offset = 0x5400C;  // SYS_SPU_THREAD_SNR1 inside a peer's 1 MiB window
constexpr u32 kSnr2Offset = 0x5C00C;  // SYS_SPU_THREAD_SNR2

template <typename... Args>
bool Bad(SpuThread& t, const char* format, Args... args) {
    if (t.ShouldStop()) return false;  // e.g. a page-fault wait cut short by group stop / process end
    char text[256];
    std::snprintf(text, sizeof(text), format, args...);
    t.Fault(text);
    return false;
}

u32 LsRead32(const SpuThread& t, u32 lsa) {
    u8* p = t.ctx.ls + (lsa & kLsMask & ~3u);
    return ByteSwap(std::atomic_ref<u32>(*reinterpret_cast<u32*>(p)).load(std::memory_order_relaxed));
}

// [ea, ea+size) not accessible in the arena: offer it to demand paging (sys_mmapper page-fault
// notification). The SPU thread blocks here until the guest pager maps the page and calls
// sys_spu_thread_recover_page_fault, then the check is repeated. False = a real fault.
bool ArenaAccessible(SpuThread& t, u32 ea, u32 size, bool write) {
    vm::GuestMemory& m = t.kernel.memory();
    const u8 need = write ? vm::kPageWritable : vm::kPageReadable;
    while (!m.Check(ea, size, need)) {
        if (u64{ea} + size - 1 > 0xFFFFFFFFull) return false;
        const u32 at = (m.Flags(ea) & need) == need ? AlignDown<u32>(static_cast<u32>(u64{ea} + size - 1), vm::kPageSize) : ea;
        t.live_wait.store(SpuThread::kWaitPageFault, std::memory_order_relaxed);
        const bool retry = t.kernel.mmapper().HandlePageFault(lv2::SYS_MEMORY_PAGE_FAULT_TYPE_SPU_THREAD, t.id, at, write,
                                                              &t.stop_requested);
        t.live_wait.store(-1, std::memory_order_relaxed);
        if (!retry) return false;
    }
    return true;
}

// Host pointer for [ea, ea+size) on the EA side of a transfer. Peer LS windows are resolved
// against the thread's own group; `keep` holds the peer alive for the copy.
u8* ResolveEa(SpuThread& t, u32 ea, u32 size, bool write, std::shared_ptr<SpuThread>& keep, bool& is_arena) {
    is_arena = false;
    if (ea >= kSpuThreadBaseLow) {
        const u32 rel = ea - kSpuThreadBaseLow, n = rel / kLsWindow, off = rel % kLsWindow;
        if (u64{off} + size <= kLsSize && (keep = t.group.Peer(n))) return keep->ctx.ls + off;
        return nullptr;
    }
    if (!ArenaAccessible(t, ea, size, write)) return nullptr;
    is_arena = true;
    return t.kernel.memory().shadow() + ea;
}

bool Transfer(SpuThread& t, u32 lsa, u32 ea, u32 size, bool put) {
    if (!size) return true;
    if (size < 16) {
        if ((size != 1 && size != 2 && size != 4 && size != 8) || (ea & (size - 1)) || (lsa & 15) != (ea & 15))
            return Bad(t, "MFC %s: misaligned small transfer (LSA 0x%05x, EA 0x%08x, size %u)", put ? "PUT" : "GET", lsa, ea, size);
    } else if ((size & 15) || size > kMaxDmaSize || (lsa & 15) || (ea & 15)) {
        return Bad(t, "MFC %s: invalid transfer (LSA 0x%05x, EA 0x%08x, size 0x%x)", put ? "PUT" : "GET", lsa, ea, size);
    }
    std::shared_ptr<SpuThread> peer;
    bool is_arena = false;
    u8* host = ResolveEa(t, ea, size, put, peer, is_arena);
    if (!host)
        return Bad(t, "MFC %s: EA 0x%08x+0x%x is not accessible (%s)", put ? "PUT" : "GET", ea, size,
                   ea >= kSpuThreadBaseLow ? "no such SPU thread in this group" : "unmapped or protected");
    lsa &= kLsMask;
    // Local store addresses wrap at 256 KiB; the EA side is contiguous.
    const u32 first = std::min(size, kLsSize - lsa);
    if (put) {
        vm::GuestCopy(host, t.ctx.ls + lsa, first);
        if (first < size) vm::GuestCopy(host + first, t.ctx.ls, size - first);
        if (is_arena && (t.kernel.memory().Flags(ea) & vm::kPageCode)) t.kernel.code().Invalidate(ea, size);
    } else {
        vm::GuestCopy(t.ctx.ls + lsa, host, first);
        if (first < size) vm::GuestCopy(t.ctx.ls, host + first, size - first);
    }
    return true;
}

bool ListTransfer(SpuThread& t, bool put) {
    // List commands: MFC_EAL = LS address of the list, MFC_Size = list size in bytes (8 per
    // element). Element: { be32 stall_and_notify(bit 31) | size(bits 0..14); be32 eal }.
    MfcState& f = t.mfc;
    const u32 list = f.eal & kLsMask & ~7u, list_size = f.size;
    if ((list_size & 7) || list_size > kMaxDmaSize)
        return Bad(t, "MFC list: invalid list size 0x%x", list_size);
    u32 lsa = f.lsa & 0x3FFF0;
    for (u32 i = 0; i < list_size / 8; ++i) {
        const u32 word0 = LsRead32(t, list + i * 8), ea = LsRead32(t, list + i * 8 + 4);
        const u32 size = word0 & 0x7FFF;
        if (size) {
            if (!Transfer(t, lsa | (ea & 15), ea, size, put)) return false;
            lsa = (lsa + AlignUp<u32>(size, 16)) & kLsMask;
        }
        if (word0 >> 31) {
            // The list is not paused: it has already completed when the SPU looks. Reporting the
            // tag as stalled keeps the usual "read stall stat, patch, ack" loop consistent.
            f.stall_mask |= 1u << (f.tag & 31);
            if (!f.stall_notify_warned) {
                f.stall_notify_warned = true;
                LOG_WARN("spu", "DMA list stall-and-notify: the list is not paused (synchronous MFC)");
            }
        }
    }
    return true;
}

bool GetLlar(SpuThread& t) {
    MfcState& f = t.mfc;
    const u32 ea = f.eal, lsa = f.lsa & kLsMask & ~(kReservationSize - 1);
    if ((ea & (kReservationSize - 1)) || !ArenaAccessible(t, ea, kReservationSize, false))
        return Bad(t, "GETLLAR: EA 0x%08x not a readable 128-byte line", ea);
    u8* line = t.kernel.memory().shadow() + ea;
    std::lock_guard lock(Stripe(ea));
    vm::GuestCopy(f.reservation_data.data(), line, kReservationSize);
    vm::GuestCopy(t.ctx.ls + lsa, f.reservation_data.data(), kReservationSize);
    f.reservation_valid = true;
    f.reservation_ea = ea;
    f.atomic_stat = MFC_GETLLAR_SUCCESS;
    f.atomic_pending = true;
    return true;
}

bool PutLlc(SpuThread& t, bool conditional) {
    MfcState& f = t.mfc;
    const u32 ea = f.eal, lsa = f.lsa & kLsMask & ~(kReservationSize - 1);
    if ((ea & (kReservationSize - 1)) || !ArenaAccessible(t, ea, kReservationSize, true))
        return Bad(t, "%s: EA 0x%08x not a writable 128-byte line", conditional ? "PUTLLC" : "PUTLLUC", ea);
    u8* line = t.kernel.memory().shadow() + ea;
    bool success = true;
    {
        std::lock_guard lock(Stripe(ea));
        if (conditional) {
            std::array<u8, kReservationSize> current;
            vm::GuestCopy(current.data(), line, kReservationSize);
            success = f.reservation_valid && f.reservation_ea == ea &&
                      std::memcmp(current.data(), f.reservation_data.data(), kReservationSize) == 0;
        }
        if (success) vm::GuestCopy(line, t.ctx.ls + lsa, kReservationSize);
    }
    if (conditional || (f.reservation_valid && f.reservation_ea == ea)) f.reservation_valid = false;
    if (success && (t.kernel.memory().Flags(ea) & vm::kPageCode)) t.kernel.code().Invalidate(ea, kReservationSize);
    f.atomic_stat = conditional ? (success ? MFC_PUTLLC_SUCCESS : MFC_PUTLLC_FAILURE) : MFC_PUTLLUC_SUCCESS;
    f.atomic_pending = true;
    return true;
}

bool SendSignal(SpuThread& t) {
    MfcState& f = t.mfc;
    const u32 ea = f.eal;
    if (f.size != 4 || (f.lsa & 15) != (ea & 15) || ea < kSpuThreadBaseLow)
        return Bad(t, "SNDSIG: unsupported target EA 0x%08x size %u", ea, f.size);
    const u32 rel = ea - kSpuThreadBaseLow, n = rel / kLsWindow, off = rel % kLsWindow;
    const auto peer = t.group.Peer(n);
    if (!peer || (off != kSnr1Offset && off != kSnr2Offset))
        return Bad(t, "SNDSIG: EA 0x%08x is not a signal register of this group", ea);
    peer->WriteSignal(off == kSnr1Offset ? 0 : 1, LsRead32(t, f.lsa));
    return true;
}

}  // namespace

bool MfcExecute(SpuThread& t, u32 cmd) {
    const MfcState& f = t.mfc;
    switch (cmd & 0xFF) {
    case MFC_PUT_CMD:
    case MFC_PUTB_CMD:
    case MFC_PUTF_CMD:
        return Transfer(t, f.lsa, f.eal, f.size, true);
    case MFC_GET_CMD:
    case MFC_GETB_CMD:
    case MFC_GETF_CMD:
        return Transfer(t, f.lsa, f.eal, f.size, false);
    case MFC_PUTL_CMD:
    case MFC_PUTLB_CMD:
    case MFC_PUTLF_CMD:
        return ListTransfer(t, true);
    case MFC_GETL_CMD:
    case MFC_GETLB_CMD:
    case MFC_GETLF_CMD:
        return ListTransfer(t, false);
    case MFC_GETLLAR_CMD:
        return GetLlar(t);
    case MFC_PUTLLC_CMD:
        return PutLlc(t, true);
    case MFC_PUTLLUC_CMD:
    case MFC_PUTQLLUC_CMD:
        return PutLlc(t, false);
    case MFC_SNDSIG_CMD:
    case MFC_SNDSIGB_CMD:
    case MFC_SNDSIGF_CMD:
        return SendSignal(t);
    case MFC_BARRIER_CMD:
    case MFC_EIEIO_CMD:
    case MFC_SYNC_CMD:
        // Commands complete in order and immediately: only host memory ordering remains.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return true;
    default:
        return Bad(t, "unknown MFC command 0x%02x", cmd & 0xFF);
    }
}

bool MfcReservationLost(SpuThread& t) {
    MfcState& f = t.mfc;
    if (!f.reservation_valid) return false;
    std::array<u8, kReservationSize> current;
    {
        std::lock_guard lock(Stripe(f.reservation_ea));
        vm::GuestCopy(current.data(), t.kernel.memory().shadow() + f.reservation_ea, kReservationSize);
    }
    if (std::memcmp(current.data(), f.reservation_data.data(), kReservationSize) == 0) return false;
    f.reservation_valid = false;
    return true;
}

}  // namespace cell::spu
