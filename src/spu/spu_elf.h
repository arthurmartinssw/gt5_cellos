// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — spu/spu_elf.h
//
// SPU program images. PS3 games embed SPU ELFs (ELF32, big-endian, EM_SPU = 23) inside the
// PPU executable and hand them to lv2 as a sys_spu_image_t: an entry point plus a list of
// segments to copy (or fill) into a fresh 256 KiB local store.
//
//   sys_spu_image_t   { be32 type; be32 entry_point; be32 segs (sys_spu_segment_t*); be32 nsegs; }
//   sys_spu_segment_t { be32 type; be32 ls_start; be32 size; be32 addr_or_value; }
//     type 1 COPY: copy `size` bytes from EA addr to LS ls_start
//     type 2 FILL: fill `size` bytes at ls_start with the 32-bit value
//     type 4 INFO: ignored at load time
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/types.h"
#include "memory/guest_memory.h"

namespace cell::spu {

inline constexpr u16 EM_SPU = 23;
inline constexpr u32 SYS_SPU_SEGMENT_TYPE_COPY = 1;
inline constexpr u32 SYS_SPU_SEGMENT_TYPE_FILL = 2;
inline constexpr u32 SYS_SPU_SEGMENT_TYPE_INFO = 4;
inline constexpr u32 SYS_SPU_IMAGE_TYPE_USER = 0;
inline constexpr u32 SYS_SPU_IMAGE_PROTECT = 0;  // sys_spu_image_import: copy the segments
inline constexpr u32 SYS_SPU_IMAGE_DIRECT = 1;   // sys_spu_image_import: reference the ELF in place

struct SpuSegment {
    u32 type = SYS_SPU_SEGMENT_TYPE_COPY;
    u32 ls_start = 0;
    u32 size = 0;
    u32 addr_or_value = 0;  // COPY: source EA; FILL: fill value
};

struct SpuElfInfo {
    u32 entry = 0;
    std::vector<SpuSegment> segments;  // COPY (p_filesz) + FILL (bss), in program-header order
};

// Parses an SPU ELF that lives in guest memory at `elf_ea`. COPY sources point into that ELF.
// Returns nullopt and fills `error` when the image is not a valid SPU program.
[[nodiscard]] std::optional<SpuElfInfo> ParseSpuElf(const vm::GuestMemory& memory, u32 elf_ea, std::string* error);

// Reads a sys_spu_image_t + its segment array from guest memory.
[[nodiscard]] std::optional<SpuElfInfo> ReadSpuImage(const vm::GuestMemory& memory, u32 image_ea, std::string* error);

// Copies/fills the segments into a local store (host pointer to 256 KiB). False on a bad
// segment (outside the LS or unreadable source).
bool LoadSegments(const vm::GuestMemory& memory, const SpuElfInfo& image, u8* ls, std::string* error);

}  // namespace cell::spu
