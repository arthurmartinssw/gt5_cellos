// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_audio.h
//
// cellAudio: the PS3 audio mixer as games see it. There is no "write samples" call: the game
// owns ring buffers in guest memory and the system reads them on a fixed clock.
//
//   * cellAudioInit allocates, for 8 ports, one ring buffer of up to 256 KiB each and a table of
//     8 big-endian u64 read indexes, all in guest memory (main area).
//   * cellAudioPortOpen(CellAudioPortParam*, u32* port): a port is nChannel (2 or 8) x nBlock
//     (2/4/8/16/32) blocks of 256 float samples per channel (interleaved, big-endian):
//     portSize = nChannel * nBlock * 256 * 4 bytes at portAddr.
//   * Every 256 samples at 48 kHz (5.333 ms) the mixer — here a host thread — consumes the block
//     at the read index of every started port, clears it, advances the index (it is the "block
//     being played" the game polls through readIndexAddr), bumps the port's tag/timestamp, and
//     sends one event to every queue registered with cellAudioSetNotifyEventQueue. Audio threads
//     block on that queue and refill the block behind the index: without these events they wait
//     forever, so the clock matters even with no host audio output yet.
//   * Host output plugs into CellAudio::Tick(): the block it clears is the block to send to
//     WASAPI / PulseAudio / CoreAudio (float32, interleaved, 48 kHz).
//
// Structures (big-endian, offsets in bytes):
//   CellAudioPortParam  { u64 nChannel @0; u64 nBlock @8; u64 attr @16; float level @24; }  32 bytes
//   CellAudioPortConfig { u32 readIndexAddr @0; u32 status @4; u64 nChannel @8; u64 nBlock @16;
//                         u32 portSize @24; u32 portAddr @28; }                              32 bytes
//   status: CELL_AUDIO_STATUS_CLOSE 0x1010, READY 1 (opened), RUN 2 (started).
// Errors and semantics follow the SDK as reproduced by RPCS3 (cellAudio.h/.cpp).
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hle/hle_registry.h"
#include "lv2/kernel.h"

namespace cell::lv2 {
struct Lv2EventQueue;
}

namespace cell::hle {

inline constexpr s32 CELL_AUDIO_ERROR_ALREADY_INIT = static_cast<s32>(0x80310701u);
inline constexpr s32 CELL_AUDIO_ERROR_AUDIOSYSTEM = static_cast<s32>(0x80310702u);
inline constexpr s32 CELL_AUDIO_ERROR_NOT_INIT = static_cast<s32>(0x80310703u);
inline constexpr s32 CELL_AUDIO_ERROR_PARAM = static_cast<s32>(0x80310704u);
inline constexpr s32 CELL_AUDIO_ERROR_PORT_FULL = static_cast<s32>(0x80310705u);
inline constexpr s32 CELL_AUDIO_ERROR_PORT_ALREADY_RUN = static_cast<s32>(0x80310706u);
inline constexpr s32 CELL_AUDIO_ERROR_PORT_NOT_OPEN = static_cast<s32>(0x80310707u);
inline constexpr s32 CELL_AUDIO_ERROR_PORT_NOT_RUN = static_cast<s32>(0x80310708u);
inline constexpr s32 CELL_AUDIO_ERROR_TRANS_EVENT = static_cast<s32>(0x80310709u);
inline constexpr s32 CELL_AUDIO_ERROR_PORT_OPEN = static_cast<s32>(0x8031070au);
inline constexpr s32 CELL_AUDIO_ERROR_EVENT_QUEUE = static_cast<s32>(0x8031070du);
inline constexpr s32 CELL_AUDIO_ERROR_TAG_NOT_FOUND = static_cast<s32>(0x8031070fu);

inline constexpr u32 CELL_AUDIO_STATUS_CLOSE = 0x1010;
inline constexpr u32 CELL_AUDIO_STATUS_READY = 1;
inline constexpr u32 CELL_AUDIO_STATUS_RUN = 2;
inline constexpr u64 CELL_AUDIO_PORTATTR_INITLEVEL = 0x1000;

inline constexpr u32 kAudioPortCount = 8;
inline constexpr u32 kAudioPortStride = 0x40000;  // 256 KiB: 8 channels x 32 blocks x 256 x 4
inline constexpr u32 kAudioBlockSamples = 256;
inline constexpr u32 kAudioSampleRate = 48000;
inline constexpr u64 kAudioPeriodNs = 1'000'000'000ull * kAudioBlockSamples / kAudioSampleRate;  // 5333333 ns
inline constexpr u64 kAudioNotifyKeyBase = 0x80004d494f323221ull;  // keys made by cellAudioCreateNotifyEventQueue

class CellAudio {
public:
    struct PortConfig {
        u32 read_index_addr = 0;
        u32 status = CELL_AUDIO_STATUS_CLOSE;
        u64 channels = 0;
        u64 blocks = 0;
        u32 size = 0;
        u32 addr = 0;
    };

    // realtime = false: no mixer thread; tests advance the clock with Tick().
    explicit CellAudio(lv2::Kernel& kernel, bool realtime = true) noexcept;
    ~CellAudio();
    CellAudio(const CellAudio&) = delete;
    CellAudio& operator=(const CellAudio&) = delete;

    void Register(HleRegistry& registry);

    // ---- API (CELL_* results) ----
    s32 Init();
    s32 Quit();
    s32 PortOpen(u64 channels, u64 blocks, u64 attr, float level, u32* port);
    s32 PortStart(u32 port);
    s32 PortStop(u32 port);
    s32 PortClose(u32 port);
    s32 GetPortConfig(u32 port, PortConfig* out);
    s32 GetPortTimestamp(u32 port, u64 tag, u64* stamp);
    s32 GetPortBlockTag(u32 port, u64 block, u64* tag);
    s32 SetPortLevel(u32 port, float level);
    s32 SetNotifyEventQueue(u64 key);
    s32 RemoveNotifyEventQueue(u64 key);
    s32 CreateNotifyEventQueue(u32* id, u64* key);
    s32 CheckPort(u32 port);  // CELL_OK if open

    // One mixer period: consume/clear the current block of every started port, advance the
    // indexes and tags, notify the queues.
    void Tick();
    [[nodiscard]] u64 ticks() const noexcept { return ticks_.load(); }
    [[nodiscard]] std::vector<std::string> Describe() const;
    lv2::Kernel& kernel() noexcept { return kernel_; }

private:
    enum class State : u8 { Closed, Opened, Started };
    struct Port {
        State state = State::Closed;
        u32 channels = 0;
        u32 blocks = 0;
        u64 attr = 0;
        float level = 1.0f;
        u64 tag = 0;        // blocks consumed since open (the block tag counter)
        u64 timestamp = 0;  // system time (us) of the last consumed block
    };
    void MixerMain();
    [[nodiscard]] u32 PortAddr(u32 n) const noexcept { return buffer_ + n * kAudioPortStride; }
    [[nodiscard]] u32 IndexAddr(u32 n) const noexcept { return indexes_ + n * 8; }

    lv2::Kernel& kernel_;
    const bool realtime_;
    mutable std::mutex mutex_;
    bool initialized_ = false;
    u32 buffer_ = 0;   // guest EA of the 8 port buffers
    u32 indexes_ = 0;  // guest EA of the 8 read indexes (be64)
    std::array<Port, kAudioPortCount> ports_{};
    struct Notify {
        u64 key;
        std::weak_ptr<lv2::Lv2EventQueue> queue;
    };
    std::vector<Notify> notify_;
    std::atomic<u64> ticks_{0};
    std::atomic<u64> events_sent_{0};
    std::atomic<bool> stop_{false};
    std::thread mixer_;
};

}  // namespace cell::hle
