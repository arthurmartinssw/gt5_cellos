// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — spu/spu_defs.h
//
// SPU architecture constants (Cell Broadband Engine Architecture: SPU channels, MFC commands,
// events) and the lv2 conventions layered on top (stop codes, SPU thread event protocol).
// Values per the CBEA / Cell Programming Handbook and lv2 as reimplemented by RPCS3.
#pragma once

#include "common/types.h"

namespace cell::spu {

inline constexpr u32 kLsSize = 0x40000;         // 256 KiB local store per SPU
inline constexpr u32 kLsMask = kLsSize - 1;     // LS addresses wrap at 256 KiB
inline constexpr u32 kLsWindow = 0x100000;      // EA window per SPU thread (lv2 SYS_SPU_THREAD_OFFSET)
inline constexpr u32 kSpuThreadBaseLow = 0xF0000000;  // SYS_SPU_THREAD_BASE_LOW
inline constexpr u32 kMaxDmaSize = 0x4000;      // 16 KiB per MFC transfer
inline constexpr u32 kReservationSize = 128;    // GETLLAR/PUTLLC line
inline constexpr u32 kInMboxDepth = 4;          // PPU -> SPU inbound mailbox entries

// ---- SPU channels (rdch / wrch / rchcnt) ----
enum Channel : u32 {
    SPU_RdEventStat = 0,
    SPU_WrEventMask = 1,
    SPU_WrEventAck = 2,
    SPU_RdSigNotify1 = 3,
    SPU_RdSigNotify2 = 4,
    SPU_WrDec = 7,
    SPU_RdDec = 8,
    MFC_WrMSSyncReq = 9,
    SPU_RdEventMask = 11,
    MFC_RdTagMask = 12,
    SPU_RdMachStat = 13,
    SPU_WrSRR0 = 14,
    SPU_RdSRR0 = 15,
    MFC_LSA = 16,
    MFC_EAH = 17,
    MFC_EAL = 18,
    MFC_Size = 19,
    MFC_TagID = 20,
    MFC_Cmd = 21,
    MFC_WrTagMask = 22,
    MFC_WrTagUpdate = 23,
    MFC_RdTagStat = 24,
    MFC_RdListStallStat = 25,
    MFC_WrListStallAck = 26,
    MFC_RdAtomicStat = 27,
    SPU_WrOutMbox = 28,
    SPU_RdInMbox = 29,
    SPU_WrOutIntrMbox = 30,
};

// ---- SPU events (SPU_RdEventStat / SPU_WrEventMask bits) ----
enum Event : u32 {
    SPU_EVENT_TG = 0x0001,  // tag-group status update
    SPU_EVENT_SN = 0x0002,  // DMA list stall-and-notify
    SPU_EVENT_QV = 0x0008,  // MFC command queue vacancy
    SPU_EVENT_MB = 0x0010,  // inbound mailbox available
    SPU_EVENT_TM = 0x0020,  // decrementer
    SPU_EVENT_ME = 0x0040,  // outbound interrupt mailbox available
    SPU_EVENT_LE = 0x0080,  // outbound mailbox available
    SPU_EVENT_S2 = 0x0100,  // signal notification 2
    SPU_EVENT_S1 = 0x0200,  // signal notification 1
    SPU_EVENT_LR = 0x0400,  // lock-line reservation lost
    SPU_EVENT_A = 0x0800,   // privileged attention
    SPU_EVENT_MS = 0x1000,  // multisource synchronization
};

// ---- MFC commands (MFC_Cmd) ----
enum MfcCommand : u32 {
    MFC_PUT_CMD = 0x20,
    MFC_PUTB_CMD = 0x21,
    MFC_PUTF_CMD = 0x22,
    MFC_PUTL_CMD = 0x24,
    MFC_PUTLB_CMD = 0x25,
    MFC_PUTLF_CMD = 0x26,
    MFC_GET_CMD = 0x40,
    MFC_GETB_CMD = 0x41,
    MFC_GETF_CMD = 0x42,
    MFC_GETL_CMD = 0x44,
    MFC_GETLB_CMD = 0x45,
    MFC_GETLF_CMD = 0x46,
    MFC_SNDSIG_CMD = 0xA0,
    MFC_SNDSIGB_CMD = 0xA1,
    MFC_SNDSIGF_CMD = 0xA2,
    MFC_PUTLLUC_CMD = 0xB0,
    MFC_PUTLLC_CMD = 0xB4,
    MFC_PUTQLLUC_CMD = 0xB8,
    MFC_SYNC_CMD = 0xCC,
    MFC_EIEIO_CMD = 0xC8,
    MFC_BARRIER_CMD = 0xC0,
    MFC_GETLLAR_CMD = 0xD0,
};

// MFC_RdAtomicStat bits
inline constexpr u32 MFC_PUTLLC_SUCCESS = 0;
inline constexpr u32 MFC_PUTLLC_FAILURE = 1;
inline constexpr u32 MFC_PUTLLUC_SUCCESS = 2;
inline constexpr u32 MFC_GETLLAR_SUCCESS = 4;

// MFC_WrTagUpdate values
inline constexpr u32 MFC_TAG_UPDATE_IMMEDIATE = 0;
inline constexpr u32 MFC_TAG_UPDATE_ANY = 1;
inline constexpr u32 MFC_TAG_UPDATE_ALL = 2;

// ---- lv2 stop-and-signal codes (SPU "stop" instruction) ----
inline constexpr u32 SYS_SPU_THREAD_STOP_YIELD = 0x0100;
inline constexpr u32 SYS_SPU_THREAD_STOP_GROUP_EXIT = 0x0101;
inline constexpr u32 SYS_SPU_THREAD_STOP_THREAD_EXIT = 0x0102;
inline constexpr u32 SYS_SPU_THREAD_STOP_RECEIVE_EVENT = 0x0110;
inline constexpr u32 SYS_SPU_THREAD_STOP_TRY_RECEIVE_EVENT = 0x0111;

// ---- lv2 SPU thread event protocol (SPU_WrOutIntrMbox value = spup << 24 | data0) ----
inline constexpr u64 SYS_SPU_THREAD_EVENT_USER_KEY = 0xFFFFFFFF53505501ull;
// Group events (sys_spu_thread_group_connect_event): source key of the event sent to the queue.
inline constexpr u64 SYS_SPU_THREAD_GROUP_EVENT_RUN_KEY = 0xFFFFFFFF53505500ull;        // data1 = group id
inline constexpr u64 SYS_SPU_THREAD_GROUP_EVENT_EXCEPTION_KEY = 0xFFFFFFFF53505503ull;  // data1 = group, data2 = thread
inline constexpr u64 SYS_SPU_THREAD_GROUP_EVENT_SYSTEM_MODULE_KEY = 0xFFFFFFFF53505504ull;
inline constexpr u32 kSpuPortCount = 64;  // send_event ports 0..63 (throw_event: 64..127 - 64)

}  // namespace cell::spu
