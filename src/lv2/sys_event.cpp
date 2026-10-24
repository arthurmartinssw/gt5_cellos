// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_event.cpp
#include "lv2/sys_event.h"

#include <algorithm>
#include <cstdio>

#include "common/log.h"
#include "lv2/sys_sync.h"

namespace cell::lv2 {

// =======================================================================================
// Queue
// =======================================================================================

std::string Lv2EventQueue::Describe() {
    char line[256];
    std::unique_lock lock(m, std::try_to_lock);
    if (!lock) {
        std::snprintf(line, sizeof(line), "event queue 0x%08x \"%s\": busy", id, name.c_str());
        return line;
    }
    char last[48] = "never";
    if (pushed) {
        const u64 now = ppu::PpuInterpreter::SystemTimeMicroseconds();
        std::snprintf(last, sizeof(last), "%.1f ms ago", static_cast<double>(now - std::min(now, last_push_us)) / 1000.0);
    }
    std::snprintf(line, sizeof(line),
                  "event queue 0x%08x \"%s\" (%s, key 0x%llx): %u waiter(s), %zu pending event(s), %llu received in total (last %s)%s",
                  id, name.c_str(), type == SYS_PPU_QUEUE ? "PPU" : "SPU", static_cast<unsigned long long>(key), waiters,
                  events.size(), static_cast<unsigned long long>(pushed), last, destroyed ? ", destroyed" : "");
    return line;
}

void Lv2EventQueue::WakeForTermination() {
    std::lock_guard lock(m);
    cv.notify_all();
}

s32 Lv2EventQueue::Push(const Lv2Event& event) {
    std::lock_guard lock(m);
    if (destroyed) return CELL_ENOTCONN;
    if (events.size() >= size) return CELL_EBUSY;
    events.push_back(event);
    ++pushed;
    last_push_us = ppu::PpuInterpreter::SystemTimeMicroseconds();
    cv.notify_all();
    return CELL_OK;
}

s32 Lv2EventQueue::Receive(Kernel& kernel, u64 timeout_us, Lv2Event* out) {
    std::unique_lock lock(m);
    if (destroyed) return CELL_ESRCH;
    if (type != SYS_PPU_QUEUE) return CELL_EINVAL;  // SPU queues are received by SPU threads
    ++waiters;
    const WaitResult r = kernel.Wait(lock, cv, timeout_us, [&] { return !events.empty() || destroyed; });
    --waiters;
    if (r == WaitResult::Terminated) return CELL_ECANCELED;
    if (destroyed) return CELL_ECANCELED;  // forced destroy while we were waiting
    if (r == WaitResult::TimedOut) return CELL_ETIMEDOUT;
    *out = events.front();
    events.pop_front();
    return CELL_OK;
}

// =======================================================================================
// Syscalls
// =======================================================================================

namespace {

using C = ppu::PpuThreadContext;
Kernel& K(void* p) { return *static_cast<Kernel*>(p); }
u32 A32(const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); }

void WriteEvent(vm::GuestMemory& m, u32 addr, const Lv2Event& e) {
    m.Write<u64>(addr + 0, e.source);
    m.Write<u64>(addr + 8, e.data1);
    m.Write<u64>(addr + 16, e.data2);
    m.Write<u64>(addr + 24, e.data3);
}

}  // namespace

void RegisterEventSyscalls(Kernel& kernel) {
    SyscallTable& t = kernel.syscalls();

    t.Register(128, "sys_event_queue_create", [](void* p, C& c) -> s32 {
        // (u32* equeue_id, sys_event_queue_attribute_t* attr, u64 key, s32 size)
        // attr: protocol +0, type +4, name[8] +8
        Kernel& k = K(p);
        vm::GuestMemory& m = k.memory();
        const u32 out = A32(c, 0), attr = A32(c, 1);
        const u64 key = c.gpr[5];
        const s32 size = static_cast<s32>(c.gpr[6]);
        if (!m.Check(out, 4, vm::kPageWritable) || !m.Check(attr, 16, vm::kPageReadable)) return CELL_EFAULT;
        if (size <= 0 || size > static_cast<s32>(kEventQueueMaxSize)) return CELL_EINVAL;
        const u32 protocol = m.Read<u32>(attr);
        const s32 type = static_cast<s32>(m.Read<u32>(attr + 4));
        if ((protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_PRIORITY) ||
            (type != SYS_PPU_QUEUE && type != SYS_SPU_QUEUE))
            return CELL_EINVAL;
        if (key) {  // IPC key: a second create with the same key must fail
            for (const auto& obj : k.objects().Snapshot())
                if (const auto q = std::dynamic_pointer_cast<Lv2EventQueue>(obj); q && q->key == key) return CELL_EEXIST;
        }
        auto q = std::make_shared<Lv2EventQueue>();
        q->protocol = protocol;
        q->type = type;
        q->key = key;
        q->size = static_cast<u32>(size);
        q->name = ReadObjectName(m, attr + 8);
        const u32 id = k.objects().Add(kIdEventQueue, q);
        m.Write<u32>(out, id);
        LOG_TRACE("lv2", "sys_event_queue_create \"%s\" -> 0x%08x (%s, size %d)", q->name.c_str(), id,
                  type == SYS_PPU_QUEUE ? "PPU" : "SPU", size);
        return CELL_OK;
    }, &kernel);

    t.Register(129, "sys_event_queue_destroy", [](void* p, C& c) -> s32 {
        // (u32 equeue_id, s32 mode): mode 1 = force (waiters get CELL_ECANCELED)
        Kernel& k = K(p);
        const u32 id = A32(c, 0);
        const s32 mode = static_cast<s32>(c.gpr[4]);
        if (mode != 0 && mode != SYS_EVENT_QUEUE_DESTROY_FORCE) return CELL_EINVAL;
        const auto q = k.objects().Get<Lv2EventQueue>(id);
        if (!q) return CELL_ESRCH;
        {
            std::lock_guard lock(q->m);
            if (q->waiters && mode != SYS_EVENT_QUEUE_DESTROY_FORCE) return CELL_EBUSY;
            q->destroyed = true;
            q->cv.notify_all();
        }
        k.objects().Remove(id);
        return CELL_OK;
    }, &kernel);

    t.Register(130, "sys_event_queue_receive", [](void* p, C& c) -> s32 {
        // (u32 equeue_id, sys_event_t* unused, u64 timeout) -> event in r4..r7
        Kernel& k = K(p);
        const auto q = k.objects().Get<Lv2EventQueue>(A32(c, 0));
        if (!q) return CELL_ESRCH;
        Lv2Event e{};
        const s32 r = q->Receive(k, c.gpr[5], &e);
        if (r == CELL_OK) {
            c.gpr[4] = e.source;
            c.gpr[5] = e.data1;
            c.gpr[6] = e.data2;
            c.gpr[7] = e.data3;
        }
        return r;
    }, &kernel);

    t.Register(131, "sys_event_queue_tryreceive", [](void* p, C& c) -> s32 {
        // (u32 equeue_id, sys_event_t* array, s32 size, u32* number)
        Kernel& k = K(p);
        vm::GuestMemory& m = k.memory();
        const u32 array = A32(c, 1), number = A32(c, 3);
        const s32 size = static_cast<s32>(c.gpr[5]);
        if (size < 0) return CELL_EINVAL;
        if (!m.Check(number, 4, vm::kPageWritable) ||
            (size && !m.Check(array, static_cast<u32>(size) * 32, vm::kPageWritable)))
            return CELL_EFAULT;
        const auto q = k.objects().Get<Lv2EventQueue>(A32(c, 0));
        if (!q) return CELL_ESRCH;
        std::lock_guard lock(q->m);
        if (q->type != SYS_PPU_QUEUE) return CELL_EINVAL;
        u32 n = 0;
        while (n < static_cast<u32>(size) && !q->events.empty()) {
            WriteEvent(m, array + n * 32, q->events.front());
            q->events.pop_front();
            ++n;
        }
        m.Write<u32>(number, n);
        return CELL_OK;
    }, &kernel);

    t.Register(133, "sys_event_queue_drain", [](void* p, C& c) -> s32 {
        const auto q = K(p).objects().Get<Lv2EventQueue>(A32(c, 0));
        if (!q) return CELL_ESRCH;
        std::lock_guard lock(q->m);
        q->events.clear();
        return CELL_OK;
    }, &kernel);

    t.Register(134, "sys_event_port_create", [](void* p, C& c) -> s32 {
        // (u32* eport_id, s32 port_type, u64 name)
        Kernel& k = K(p);
        const u32 out = A32(c, 0);
        if (!k.memory().Check(out, 4, vm::kPageWritable)) return CELL_EFAULT;
        if (static_cast<s32>(c.gpr[4]) != SYS_EVENT_PORT_LOCAL) return CELL_EINVAL;
        auto port = std::make_shared<Lv2EventPort>();
        port->port_name = c.gpr[5];
        k.memory().Write<u32>(out, k.objects().Add(kIdEventPort, port));
        return CELL_OK;
    }, &kernel);

    t.Register(135, "sys_event_port_destroy", [](void* p, C& c) -> s32 {
        Kernel& k = K(p);
        const u32 id = A32(c, 0);
        const auto port = k.objects().Get<Lv2EventPort>(id);
        if (!port) return CELL_ESRCH;
        {
            std::lock_guard lock(port->m);
            if (port->queue.lock()) return CELL_EISCONN;
        }
        k.objects().Remove(id);
        return CELL_OK;
    }, &kernel);

    t.Register(136, "sys_event_port_connect_local", [](void* p, C& c) -> s32 {
        // (u32 eport_id, u32 equeue_id)
        Kernel& k = K(p);
        const auto port = k.objects().Get<Lv2EventPort>(A32(c, 0));
        const auto q = k.objects().Get<Lv2EventQueue>(A32(c, 1));
        if (!port || !q) return CELL_ESRCH;
        std::lock_guard lock(port->m);
        if (port->type != SYS_EVENT_PORT_LOCAL) return CELL_EINVAL;
        if (port->queue.lock()) return CELL_EISCONN;
        port->queue = q;
        return CELL_OK;
    }, &kernel);

    t.Register(137, "sys_event_port_disconnect", [](void* p, C& c) -> s32 {
        const auto port = K(p).objects().Get<Lv2EventPort>(A32(c, 0));
        if (!port) return CELL_ESRCH;
        std::lock_guard lock(port->m);
        if (!port->queue.lock()) return CELL_ENOTCONN;
        port->queue.reset();
        return CELL_OK;
    }, &kernel);

    t.Register(138, "sys_event_port_send", [](void* p, C& c) -> s32 {
        // (u32 eport_id, u64 data1, u64 data2, u64 data3)
        const auto port = K(p).objects().Get<Lv2EventPort>(A32(c, 0));
        if (!port) return CELL_ESRCH;
        std::shared_ptr<Lv2EventQueue> q;
        u64 source;
        {
            std::lock_guard lock(port->m);
            q = port->queue.lock();
            // Unnamed ports identify themselves as (process id << 32) | port id.
            source = port->port_name ? port->port_name : (u64{kProcessId} << 32) | port->id;
        }
        if (!q) return CELL_ENOTCONN;
        return q->Push({source, c.gpr[4], c.gpr[5], c.gpr[6]});
    }, &kernel);
}

}  // namespace cell::lv2
