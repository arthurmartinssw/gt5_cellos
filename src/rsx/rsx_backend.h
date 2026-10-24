// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — rsx/rsx_backend.h
//
// Split between the RSX *front end* (rsx_fifo: walks the command buffer, follows jumps/calls,
// performs references, semaphores and flips — everything the PPU synchronizes on) and the
// *back end* that turns graphics methods into host GPU work.
//
// The Vulkan renderer (next GPU phase) implements IRsxBackend:
//   OnMethod  — writes the NV4097 register file (surface setup from NV4097_SET_SURFACE_FORMAT
//               0x0208, viewport 0x0A00+, scissor 0x08C0+, vertex arrays 0x1680/0x1740,
//               textures 0x1A00+, fragment program 0x08E4, vertex program upload 0x1E9C,
//               blend/depth/stencil state) and executes the "action" methods:
//               NV4097_SET_BEGIN_END (0x1808) closes a draw -> vkCmdDraw[Indexed];
//               NV4097_CLEAR_SURFACE (0x1D94) -> vkCmdClearAttachments; NV3089/NV0039
//               transfers -> vkCmdCopy*. State becomes VkPipeline keys and descriptor sets;
//               RSX fragment/vertex programs are translated to SPIR-V.
//   OnFlip    — presents the display buffer registered with cellGcmSetDisplayBuffer
//               (vkQueuePresentKHR on the swapchain).
// Both run on the RSX front-end thread, in command-buffer order.
#pragma once

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "common/types.h"

namespace cell::rsx {

class IRsxBackend {
public:
    virtual ~IRsxBackend() = default;
    // Every method the front end does not consume itself, in FIFO order.
    virtual void OnMethod(u32 reg, u32 value) = 0;
    // A flip reached the front of the FIFO: present display buffer `buffer_id`.
    virtual void OnFlip(u32 buffer_id) = 0;
    // Images actually handed to a host surface (vkQueuePresentKHR succeeded). A back end that
    // presents nothing reports 0: a flip request is not a presented frame.
    [[nodiscard]] virtual u64 FramesPresented() const noexcept { return 0; }
};

// No GPU yet: counts what the game sends so we know which methods the renderer needs first.
// It rasterizes nothing and presents nothing (FramesPresented() stays 0).
class NullRsxBackend final : public IRsxBackend {
public:
    void OnMethod(u32 reg, u32) override {
        counts_[(reg & 0xFFFC) >> 2].fetch_add(1, std::memory_order_relaxed);
    }
    void OnFlip(u32) override { flips_.fetch_add(1, std::memory_order_relaxed); }

    [[nodiscard]] u64 flips() const noexcept { return flips_.load(std::memory_order_relaxed); }
    [[nodiscard]] u64 count(u32 reg) const noexcept { return counts_[(reg & 0xFFFC) >> 2].load(std::memory_order_relaxed); }
    // The `limit` most frequent registers as "0xRRRR x N" lines.
    [[nodiscard]] std::vector<std::string> TopMethods(std::size_t limit) const;

private:
    std::array<std::atomic<u64>, 0x4000> counts_{};
    std::atomic<u64> flips_{0};
};

}  // namespace cell::rsx
