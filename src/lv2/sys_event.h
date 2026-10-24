// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_event.h
//
// lv2 event queues and event ports. Syscalls (lv2 numbering):
//   128 sys_event_queue_create     129 sys_event_queue_destroy   130 sys_event_queue_receive
//   131 sys_event_queue_tryreceive 133 sys_event_queue_drain
//   134 sys_event_port_create      135 sys_event_port_destroy    136 sys_event_port_connect_local
//   137 sys_event_port_disconnect  138 sys_event_port_send
//
// A queue is a bounded FIFO (1..127 entries) of sys_event_t {source, data1, data2, data3}.
// Producers are ports (PPU), SPU threads (sys_spu_thread_send_event, phase 4) and the kernel
// itself (cellAudio/cellGcm notifications later). For a PPU queue, receive returns the event
// in r4..r7 — the sys_event_t* argument is ignored, which is easy to get wrong.
//
// Ports hold a weak_ptr to their queue: destroying a queue silently disconnects them, and a
// later send returns CELL_ENOTCONN instead of touching a dead object.
#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

#include "lv2/kernel.h"

namespace cell::lv2 {

inline constexpr s32 SYS_PPU_QUEUE = 1;
inline constexpr s32 SYS_SPU_QUEUE = 2;
inline constexpr s32 SYS_EVENT_QUEUE_DESTROY_FORCE = 1;
inline constexpr s32 SYS_EVENT_PORT_LOCAL = 1;
inline constexpr u32 kEventQueueMaxSize = 127;

struct Lv2Event {
    u64 source;
    u64 data1;
    u64 data2;
    u64 data3;
};

struct Lv2EventQueue : Lv2Object {
    std::mutex m;
    std::condition_variable cv;
    u32 protocol = SYS_SYNC_PRIORITY;
    s32 type = SYS_PPU_QUEUE;
    u64 key = 0;
    u32 size = kEventQueueMaxSize;
    std::deque<Lv2Event> events;
    u32 waiters = 0;
    bool destroyed = false;
    u64 pushed = 0;        // events accepted (diagnostics)
    u64 last_push_us = 0;  // system time of the last accepted event

    void WakeForTermination() override;
    std::string Describe() override;
    // Called by ports, SPU threads and HLE producers. CELL_EBUSY when full.
    s32 Push(const Lv2Event& event);
    s32 Receive(Kernel& kernel, u64 timeout_us, Lv2Event* out);
};

struct Lv2EventPort : Lv2Object {
    std::mutex m;
    s32 type = SYS_EVENT_PORT_LOCAL;
    u64 port_name = 0;
    std::weak_ptr<Lv2EventQueue> queue;
};

void RegisterEventSyscalls(Kernel& kernel);

}  // namespace cell::lv2
