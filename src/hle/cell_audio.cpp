// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_audio.cpp
#include "hle/cell_audio.h"

#include <bit>
#include <chrono>
#include <cinttypes>
#include <cstdio>

#include "common/host_thread.h"
#include "common/log.h"
#include "lv2/sys_event.h"

namespace cell::hle {

namespace {

std::shared_ptr<lv2::Lv2EventQueue> FindQueueByKey(lv2::Kernel& k, u64 key) {
    if (!key) return nullptr;
    for (const auto& obj : k.objects().Snapshot())
        if (const auto q = std::dynamic_pointer_cast<lv2::Lv2EventQueue>(obj); q && q->key == key) {
            std::lock_guard lock(q->m);
            if (!q->destroyed) return q;
        }
    return nullptr;
}

bool ValidBlocks(u64 n) { return n == 2 || n == 4 || n == 8 || n == 16 || n == 32; }

// The read index is polled by guest threads while the mixer updates it. The PPU interpreter
// reads aligned guest words through relaxed atomic_ref (ppu_interpreter.cpp), so the mixer
// stores the index the same way: one 8-byte atomic store, never a torn value.
void StoreIndex(vm::GuestMemory& m, u32 ea, u64 value) {
    std::atomic_ref<u64>(*reinterpret_cast<u64*>(m.shadow() + ea)).store(ByteSwap(value), std::memory_order_release);
}

}  // namespace

CellAudio::CellAudio(lv2::Kernel& kernel, bool realtime) noexcept : kernel_(kernel), realtime_(realtime) {}

CellAudio::~CellAudio() {
    stop_.store(true);
    if (mixer_.joinable()) mixer_.join();
}

// ---------------------------------------------------------------------------------------
// Init / quit
// ---------------------------------------------------------------------------------------

s32 CellAudio::Init() {
    std::lock_guard lock(mutex_);
    if (initialized_) return CELL_AUDIO_ERROR_ALREADY_INIT;
    vm::GuestMemory& m = kernel_.memory();
    if (!buffer_) {  // kept across Quit/Init: the game may still hold the addresses
        const auto buffer = m.Allocate(vm::Area::Main, u64{kAudioPortCount} * kAudioPortStride, vm::k64K, vm::kProtReadWrite);
        const auto indexes = m.Allocate(vm::Area::Main, vm::k64K, vm::k64K, vm::kProtReadWrite);
        if (!buffer || !indexes) return CELL_AUDIO_ERROR_AUDIOSYSTEM;
        buffer_ = *buffer;
        indexes_ = *indexes;
    }
    m.Fill(buffer_, 0, u64{kAudioPortCount} * kAudioPortStride);
    m.Fill(indexes_, 0, kAudioPortCount * 8);
    ports_ = {};
    initialized_ = true;
    LOG_INFO("cellAudio", "cellAudioInit: port buffers at 0x%08x, read indexes at 0x%08x, %s", buffer_, indexes_,
             realtime_ ? "mixer clock 5.333 ms (no host output yet)" : "manual clock");
    if (realtime_ && !mixer_.joinable()) {
        stop_.store(false);
        mixer_ = std::thread(&CellAudio::MixerMain, this);
    }
    return CELL_OK;
}

s32 CellAudio::Quit() {
    {
        std::lock_guard lock(mutex_);
        if (!initialized_) return CELL_AUDIO_ERROR_NOT_INIT;
        initialized_ = false;
        ports_ = {};
        notify_.clear();
    }
    stop_.store(true);
    if (mixer_.joinable()) mixer_.join();
    return CELL_OK;
}

// ---------------------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------------------

s32 CellAudio::PortOpen(u64 channels, u64 blocks, u64 attr, float level, u32* port) {
    std::lock_guard lock(mutex_);
    if (!initialized_) return CELL_AUDIO_ERROR_NOT_INIT;
    if ((channels != 2 && channels != 8) || !ValidBlocks(blocks)) {
        LOG_WARN("cellAudio", "cellAudioPortOpen: unsupported %" PRIu64 " channel(s) x %" PRIu64 " block(s)", channels, blocks);
        return CELL_AUDIO_ERROR_PARAM;
    }
    for (u32 n = 0; n < kAudioPortCount; ++n) {
        Port& p = ports_[n];
        if (p.state != State::Closed) continue;
        p = Port{};
        p.state = State::Opened;
        p.channels = static_cast<u32>(channels);
        p.blocks = static_cast<u32>(blocks);
        p.attr = attr;
        p.level = (attr & CELL_AUDIO_PORTATTR_INITLEVEL) ? level : 1.0f;
        p.timestamp = ppu::PpuInterpreter::SystemTimeMicroseconds();
        kernel_.memory().Fill(PortAddr(n), 0, kAudioPortStride);
        StoreIndex(kernel_.memory(), IndexAddr(n), 0);
        *port = n;
        LOG_INFO("cellAudio", "cellAudioPortOpen -> port %u: %u ch x %u blocks, buffer 0x%08x (0x%x bytes), attr 0x%" PRIx64, n,
                 p.channels, p.blocks, PortAddr(n), p.channels * p.blocks * kAudioBlockSamples * 4, attr);
        return CELL_OK;
    }
    return CELL_AUDIO_ERROR_PORT_FULL;
}

s32 CellAudio::CheckPort(u32 port) {
    std::lock_guard lock(mutex_);
    if (!initialized_) return CELL_AUDIO_ERROR_NOT_INIT;
    if (port >= kAudioPortCount) return CELL_AUDIO_ERROR_PARAM;
    return ports_[port].state == State::Closed ? CELL_AUDIO_ERROR_PORT_NOT_OPEN : CELL_OK;
}

s32 CellAudio::PortStart(u32 port) {
    std::lock_guard lock(mutex_);
    if (!initialized_) return CELL_AUDIO_ERROR_NOT_INIT;
    if (port >= kAudioPortCount) return CELL_AUDIO_ERROR_PARAM;
    Port& p = ports_[port];
    if (p.state == State::Closed) return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (p.state == State::Started) return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;
    p.state = State::Started;
    return CELL_OK;
}

s32 CellAudio::PortStop(u32 port) {
    std::lock_guard lock(mutex_);
    if (!initialized_) return CELL_AUDIO_ERROR_NOT_INIT;
    if (port >= kAudioPortCount) return CELL_AUDIO_ERROR_PARAM;
    Port& p = ports_[port];
    if (p.state == State::Closed) return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (p.state != State::Started) return CELL_AUDIO_ERROR_PORT_NOT_RUN;
    p.state = State::Opened;
    return CELL_OK;
}

s32 CellAudio::PortClose(u32 port) {
    std::lock_guard lock(mutex_);
    if (!initialized_) return CELL_AUDIO_ERROR_NOT_INIT;
    if (port >= kAudioPortCount) return CELL_AUDIO_ERROR_PARAM;
    if (ports_[port].state == State::Closed) return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    ports_[port] = Port{};
    return CELL_OK;
}

s32 CellAudio::GetPortConfig(u32 port, PortConfig* out) {
    std::lock_guard lock(mutex_);
    if (!initialized_) return CELL_AUDIO_ERROR_NOT_INIT;
    if (port >= kAudioPortCount) return CELL_AUDIO_ERROR_PARAM;
    const Port& p = ports_[port];
    out->read_index_addr = IndexAddr(port);
    out->status = p.state == State::Closed ? CELL_AUDIO_STATUS_CLOSE : p.state == State::Opened ? CELL_AUDIO_STATUS_READY : CELL_AUDIO_STATUS_RUN;
    out->channels = p.channels;
    out->blocks = p.blocks;
    out->size = p.channels * p.blocks * kAudioBlockSamples * 4;
    out->addr = PortAddr(port);
    return CELL_OK;
}

s32 CellAudio::GetPortTimestamp(u32 port, u64 tag, u64* stamp) {
    std::lock_guard lock(mutex_);
    if (!initialized_) return CELL_AUDIO_ERROR_NOT_INIT;
    if (port >= kAudioPortCount) return CELL_AUDIO_ERROR_PARAM;
    const Port& p = ports_[port];
    if (p.state == State::Closed) return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (tag > p.tag) return CELL_AUDIO_ERROR_TAG_NOT_FOUND;  // not played yet
    const u64 back = (p.tag - tag) * kAudioPeriodNs / 1000;
    *stamp = p.timestamp > back ? p.timestamp - back : 0;
    return CELL_OK;
}

// Tag of block `block`: the tag counter value at which the mixer reads that block next.
s32 CellAudio::GetPortBlockTag(u32 port, u64 block, u64* tag) {
    std::lock_guard lock(mutex_);
    if (!initialized_) return CELL_AUDIO_ERROR_NOT_INIT;
    if (port >= kAudioPortCount) return CELL_AUDIO_ERROR_PARAM;
    const Port& p = ports_[port];
    if (p.state == State::Closed) return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (block >= p.blocks) return CELL_AUDIO_ERROR_PARAM;
    const u64 cur = p.tag % p.blocks;
    *tag = p.tag + (block + p.blocks - cur) % p.blocks;
    return CELL_OK;
}

s32 CellAudio::SetPortLevel(u32 port, float level) {
    std::lock_guard lock(mutex_);
    if (!initialized_) return CELL_AUDIO_ERROR_NOT_INIT;
    if (port >= kAudioPortCount || level < 0.0f) return CELL_AUDIO_ERROR_PARAM;
    if (ports_[port].state == State::Closed) return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    ports_[port].level = level;
    return CELL_OK;
}

// ---------------------------------------------------------------------------------------
// Notify queues
// ---------------------------------------------------------------------------------------

s32 CellAudio::SetNotifyEventQueue(u64 key) {
    const auto queue = FindQueueByKey(kernel_, key);
    std::lock_guard lock(mutex_);
    if (!initialized_) return CELL_AUDIO_ERROR_NOT_INIT;
    if (!queue) {
        LOG_WARN("cellAudio", "cellAudioSetNotifyEventQueue(0x%016" PRIx64 "): no event queue with that key", key);
        return CELL_AUDIO_ERROR_TRANS_EVENT;
    }
    for (const Notify& n : notify_)
        if (n.key == key) return CELL_OK;
    notify_.push_back({key, queue});
    LOG_INFO("cellAudio", "notify queue 0x%08x (key 0x%016" PRIx64 ") registered", queue->id, key);
    return CELL_OK;
}

s32 CellAudio::RemoveNotifyEventQueue(u64 key) {
    std::lock_guard lock(mutex_);
    if (!initialized_) return CELL_AUDIO_ERROR_NOT_INIT;
    const auto before = notify_.size();
    std::erase_if(notify_, [&](const Notify& n) { return n.key == key; });
    return notify_.size() == before ? CELL_AUDIO_ERROR_TRANS_EVENT : CELL_OK;
}

s32 CellAudio::CreateNotifyEventQueue(u32* id, u64* key) {
    {
        std::lock_guard lock(mutex_);
        if (!initialized_) return CELL_AUDIO_ERROR_NOT_INIT;
    }
    for (u64 k = kAudioNotifyKeyBase; k < kAudioNotifyKeyBase + 0x100; ++k) {
        if (FindQueueByKey(kernel_, k)) continue;
        auto q = std::make_shared<lv2::Lv2EventQueue>();
        q->protocol = lv2::SYS_SYNC_FIFO;
        q->type = lv2::SYS_PPU_QUEUE;
        q->key = k;
        q->size = 32;
        q->name = "AudioEQ";
        *id = kernel_.objects().Add(lv2::kIdEventQueue, q);
        *key = k;
        return CELL_OK;
    }
    return CELL_AUDIO_ERROR_EVENT_QUEUE;
}

// ---------------------------------------------------------------------------------------
// Mixer clock
// ---------------------------------------------------------------------------------------

void CellAudio::Tick() {
    std::vector<std::shared_ptr<lv2::Lv2EventQueue>> queues;
    {
        std::lock_guard lock(mutex_);
        if (!initialized_) return;
        vm::GuestMemory& m = kernel_.memory();
        const u64 now = ppu::PpuInterpreter::SystemTimeMicroseconds();
        for (u32 n = 0; n < kAudioPortCount; ++n) {
            Port& p = ports_[n];
            if (p.state != State::Started) continue;
            const u32 block_bytes = p.channels * kAudioBlockSamples * 4;
            const u32 cur = static_cast<u32>(p.tag % p.blocks);
            // A host backend would take the block here (float32 BE, interleaved, x level).
            m.Fill(PortAddr(n) + cur * block_bytes, 0, block_bytes);  // played: silence until refilled
            ++p.tag;
            p.timestamp = now;
            StoreIndex(m, IndexAddr(n), p.tag % p.blocks);
        }
        for (const Notify& n : notify_)
            if (auto q = n.queue.lock()) queues.push_back(std::move(q));
    }
    ticks_.fetch_add(1, std::memory_order_relaxed);
    for (const auto& q : queues)
        if (q->Push({0, 0, 0, 0}) == CELL_OK) events_sent_.fetch_add(1, std::memory_order_relaxed);
}

void CellAudio::MixerMain() {
    host::SetCurrentThreadName("cellAudio mixer");
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(kAudioPeriodNs);
    auto next = Clock::now() + period;
    while (!stop_.load(std::memory_order_acquire) && !kernel_.is_terminating()) {
        const auto now = Clock::now();
        if (now < next) {
            host::SleepMicroseconds(static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(next - now).count()));
            continue;
        }
        Tick();
        next += period;
        if (Clock::now() - next > 8 * period) next = Clock::now() + period;  // fell behind (debugger, load): resync, no burst
    }
}

std::vector<std::string> CellAudio::Describe() const {
    std::vector<std::string> out;
    char line[200];
    std::lock_guard lock(mutex_);
    if (!initialized_) return out;
    std::snprintf(line, sizeof(line), "%" PRIu64 " mixer period(s), %" PRIu64 " notify event(s), %zu notify queue(s)", ticks_.load(),
                  events_sent_.load(), notify_.size());
    out.emplace_back(line);
    for (u32 n = 0; n < kAudioPortCount; ++n) {
        const Port& p = ports_[n];
        if (p.state == State::Closed) continue;
        std::snprintf(line, sizeof(line), "port %u: %u ch x %u blocks at 0x%08x, %s, tag %" PRIu64 ", read index %" PRIu64, n,
                      p.channels, p.blocks, PortAddr(n), p.state == State::Started ? "running" : "stopped", p.tag,
                      p.blocks ? p.tag % p.blocks : 0);
        out.emplace_back(line);
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// HLE exports
// ---------------------------------------------------------------------------------------

namespace {

using C = ppu::PpuThreadContext;
using M = vm::GuestMemory;

u32 A32(const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); }
void Return(C& c, s32 v) { c.gpr[3] = static_cast<u64>(static_cast<s64>(v)); }
CellAudio& Self(const HleFunction& f) { return *static_cast<CellAudio*>(f.user); }
bool Writable(const M& m, u32 ea, u32 size) { return ea && m.Check(ea, size, vm::kPageWritable); }
// PPU ABI: float arguments arrive in f1.., as doubles.
float FloatArg(const C& c, int i) { return static_cast<float>(c.fpr[1 + i]); }

void cellAudioInit(C& c, M&, const HleFunction& f) { Return(c, Self(f).Init()); }
void cellAudioQuit(C& c, M&, const HleFunction& f) { Return(c, Self(f).Quit()); }

// cellAudioPortOpen(CellAudioPortParam* param, u32* portNum)
void cellAudioPortOpen(C& c, M& m, const HleFunction& f) {
    const u32 param = A32(c, 0), out = A32(c, 1);
    if (!param || !m.Check(param, 28, vm::kPageReadable) || !Writable(m, out, 4)) return Return(c, CELL_AUDIO_ERROR_PARAM);
    const float level = std::bit_cast<float>(m.Read<u32>(param + 24));
    u32 port = 0;
    const s32 r = Self(f).PortOpen(m.Read<u64>(param), m.Read<u64>(param + 8), m.Read<u64>(param + 16), level, &port);
    if (r == CELL_OK) m.Write<u32>(out, port);
    Return(c, r);
}

void cellAudioPortStart(C& c, M&, const HleFunction& f) { Return(c, Self(f).PortStart(A32(c, 0))); }
void cellAudioPortStop(C& c, M&, const HleFunction& f) { Return(c, Self(f).PortStop(A32(c, 0))); }
void cellAudioPortClose(C& c, M&, const HleFunction& f) { Return(c, Self(f).PortClose(A32(c, 0))); }

// cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
void cellAudioGetPortConfig(C& c, M& m, const HleFunction& f) {
    const u32 out = A32(c, 1);
    if (!Writable(m, out, 32)) return Return(c, CELL_AUDIO_ERROR_PARAM);
    CellAudio::PortConfig cfg;
    const s32 r = Self(f).GetPortConfig(A32(c, 0), &cfg);
    if (r == CELL_OK) {
        m.Write<u32>(out + 0, cfg.read_index_addr);
        m.Write<u32>(out + 4, cfg.status);
        m.Write<u64>(out + 8, cfg.channels);
        m.Write<u64>(out + 16, cfg.blocks);
        m.Write<u32>(out + 24, cfg.size);
        m.Write<u32>(out + 28, cfg.addr);
    }
    Return(c, r);
}

// cellAudioGetPortTimestamp(u32 portNum, u64 tag, u64* stamp)
void cellAudioGetPortTimestamp(C& c, M& m, const HleFunction& f) {
    const u32 out = A32(c, 2);
    if (!Writable(m, out, 8)) return Return(c, CELL_AUDIO_ERROR_PARAM);
    u64 stamp = 0;
    const s32 r = Self(f).GetPortTimestamp(A32(c, 0), c.gpr[4], &stamp);
    if (r == CELL_OK) m.Write<u64>(out, stamp);
    Return(c, r);
}

// cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, u64* tag)
void cellAudioGetPortBlockTag(C& c, M& m, const HleFunction& f) {
    const u32 out = A32(c, 2);
    if (!Writable(m, out, 8)) return Return(c, CELL_AUDIO_ERROR_PARAM);
    u64 tag = 0;
    const s32 r = Self(f).GetPortBlockTag(A32(c, 0), c.gpr[4], &tag);
    if (r == CELL_OK) m.Write<u64>(out, tag);
    Return(c, r);
}

// cellAudioSetPortLevel(u32 portNum, float level)
void cellAudioSetPortLevel(C& c, M&, const HleFunction& f) { Return(c, Self(f).SetPortLevel(A32(c, 0), FloatArg(c, 0))); }

// cellAudioSetNotifyEventQueue(u64 key) / ...Ex(u64 key, u32 iFlags)
void cellAudioSetNotifyEventQueue(C& c, M&, const HleFunction& f) { Return(c, Self(f).SetNotifyEventQueue(c.gpr[3])); }
void cellAudioRemoveNotifyEventQueue(C& c, M&, const HleFunction& f) { Return(c, Self(f).RemoveNotifyEventQueue(c.gpr[3])); }

// cellAudioCreateNotifyEventQueue(u32* id, u64* key) / ...Ex(u32* id, u64* key, u32 iFlags)
void cellAudioCreateNotifyEventQueue(C& c, M& m, const HleFunction& f) {
    const u32 id_out = A32(c, 0), key_out = A32(c, 1);
    if (!Writable(m, id_out, 4) || !Writable(m, key_out, 8)) return Return(c, CELL_AUDIO_ERROR_PARAM);
    u32 id = 0;
    u64 key = 0;
    const s32 r = Self(f).CreateNotifyEventQueue(&id, &key);
    if (r == CELL_OK) {
        m.Write<u32>(id_out, id);
        m.Write<u64>(key_out, key);
    }
    Return(c, r);
}

// cellAudioAddData(port, float* src, u32 samples, float volume) and the 2ch / 6ch variants:
// mixing into the port happens in the host backend; validate the port and accept.
void port_data(C& c, M&, const HleFunction& f) { Return(c, Self(f).CheckPort(A32(c, 0))); }

void accept(C& c, M&, const HleFunction& f) {
    LOG_TRACE("cellAudio", "%s: accepted", f.name.c_str());
    Return(c, CELL_OK);
}

}  // namespace

void CellAudio::Register(HleRegistry& r) {
    struct Entry {
        const char* name;
        HleFunc fn;
    };
    static constexpr Entry kEntries[] = {
        {"cellAudioInit", cellAudioInit},
        {"cellAudioQuit", cellAudioQuit},
        {"cellAudioPortOpen", cellAudioPortOpen},
        {"cellAudioPortStart", cellAudioPortStart},
        {"cellAudioPortStop", cellAudioPortStop},
        {"cellAudioPortClose", cellAudioPortClose},
        {"cellAudioGetPortConfig", cellAudioGetPortConfig},
        {"cellAudioGetPortTimestamp", cellAudioGetPortTimestamp},
        {"cellAudioGetPortBlockTag", cellAudioGetPortBlockTag},
        {"cellAudioSetPortLevel", cellAudioSetPortLevel},
        {"cellAudioSetNotifyEventQueue", cellAudioSetNotifyEventQueue},
        {"cellAudioSetNotifyEventQueueEx", cellAudioSetNotifyEventQueue},
        {"cellAudioRemoveNotifyEventQueue", cellAudioRemoveNotifyEventQueue},
        {"cellAudioRemoveNotifyEventQueueEx", cellAudioRemoveNotifyEventQueue},
        {"cellAudioCreateNotifyEventQueue", cellAudioCreateNotifyEventQueue},
        {"cellAudioCreateNotifyEventQueueEx", cellAudioCreateNotifyEventQueue},
        {"cellAudioAddData", port_data},
        {"cellAudioAdd2chData", port_data},
        {"cellAudioAdd6chData", port_data},
        {"cellAudioMiscSetAccessoryVolume", accept},
        {"cellAudioSendAck", accept},
        {"cellAudioSetPersonalDevice", accept},
        {"cellAudioUnsetPersonalDevice", accept},
    };
    for (const Entry& e : kEntries) r.Register("cellAudio", e.name, e.fn, this);
}

}  // namespace cell::hle
