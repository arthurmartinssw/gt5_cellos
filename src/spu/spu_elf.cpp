// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — spu/spu_elf.cpp
#include "spu/spu_elf.h"

#include <cstdio>
#include <cstring>

#include "spu/spu_defs.h"

namespace cell::spu {

namespace {
template <typename... Args>
bool Fail(std::string* error, const char* format, Args... args) {
    if (error) {
        char text[256];
        if constexpr (sizeof...(Args) == 0) std::snprintf(text, sizeof(text), "%s", format);
        else std::snprintf(text, sizeof(text), format, args...);
        *error = text;
    }
    return false;
}
}  // namespace

std::optional<SpuElfInfo> ParseSpuElf(const vm::GuestMemory& m, u32 ea, std::string* error) {
    // ELF32 header (big-endian): e_ident[16], e_type +16, e_machine +18, e_version +20,
    // e_entry +24, e_phoff +28, e_shoff +32, e_flags +36, e_ehsize +40, e_phentsize +42, e_phnum +44
    if (!m.Check(ea, 52, vm::kPageReadable)) return Fail(error, "SPU ELF header at 0x%08x unreadable", ea), std::nullopt;
    const u8* h = m.shadow() + ea;
    if (h[0] != 0x7F || h[1] != 'E' || h[2] != 'L' || h[3] != 'F')
        return Fail(error, "no ELF magic at 0x%08x", ea), std::nullopt;
    if (h[4] != 1 || h[5] != 2) return Fail(error, "SPU ELF must be ELF32 big-endian (class %u, data %u)", h[4], h[5]), std::nullopt;
    const u16 machine = m.Read<u16>(ea + 18);
    if (machine != EM_SPU) return Fail(error, "e_machine %u is not EM_SPU (23)", machine), std::nullopt;
    SpuElfInfo info;
    info.entry = m.Read<u32>(ea + 24);
    const u32 phoff = m.Read<u32>(ea + 28);
    const u16 phentsize = m.Read<u16>(ea + 42), phnum = m.Read<u16>(ea + 44);
    if (phentsize != 32 || !phnum || phnum > 64) return Fail(error, "bad SPU program headers (%u x %u)", phnum, phentsize), std::nullopt;
    if (!m.Check(ea + phoff, phnum * 32u, vm::kPageReadable)) return Fail(error, "SPU program headers unreadable"), std::nullopt;
    for (u32 i = 0; i < phnum; ++i) {
        // Elf32_Phdr: p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align
        const u32 ph = ea + phoff + i * 32;
        const u32 type = m.Read<u32>(ph), offset = m.Read<u32>(ph + 4), vaddr = m.Read<u32>(ph + 8);
        const u32 filesz = m.Read<u32>(ph + 16), memsz = m.Read<u32>(ph + 20);
        if (type != 1 || !memsz) continue;  // PT_LOAD only
        if (filesz > memsz || u64{vaddr} + memsz > kLsSize)
            return Fail(error, "SPU segment %u [0x%x, +0x%x) does not fit the 256 KiB local store", i, vaddr, memsz), std::nullopt;
        if (filesz) info.segments.push_back({SYS_SPU_SEGMENT_TYPE_COPY, vaddr, filesz, ea + offset});
        if (memsz > filesz) info.segments.push_back({SYS_SPU_SEGMENT_TYPE_FILL, vaddr + filesz, memsz - filesz, 0});
    }
    if (info.entry >= kLsSize || (info.entry & 3)) return Fail(error, "SPU entry 0x%x invalid", info.entry), std::nullopt;
    return info;
}

std::optional<SpuElfInfo> ReadSpuImage(const vm::GuestMemory& m, u32 image_ea, std::string* error) {
    if (!m.Check(image_ea, 16, vm::kPageReadable)) return Fail(error, "sys_spu_image_t at 0x%08x unreadable", image_ea), std::nullopt;
    SpuElfInfo info;
    info.entry = m.Read<u32>(image_ea + 4);
    const u32 segs = m.Read<u32>(image_ea + 8);
    const s32 nsegs = static_cast<s32>(m.Read<u32>(image_ea + 12));
    if (nsegs < 0 || nsegs > 256 || (nsegs && !m.Check(segs, static_cast<u32>(nsegs) * 16, vm::kPageReadable)))
        return Fail(error, "sys_spu_image_t has a bad segment array (%d at 0x%08x)", nsegs, segs), std::nullopt;
    for (s32 i = 0; i < nsegs; ++i) {
        const u32 s = segs + static_cast<u32>(i) * 16;
        info.segments.push_back({m.Read<u32>(s), m.Read<u32>(s + 4), m.Read<u32>(s + 8), m.Read<u32>(s + 12)});
    }
    return info;
}

bool LoadSegments(const vm::GuestMemory& m, const SpuElfInfo& image, u8* ls, std::string* error) {
    for (const SpuSegment& s : image.segments) {
        if (s.type == SYS_SPU_SEGMENT_TYPE_INFO) continue;
        if (u64{s.ls_start} + s.size > kLsSize)
            return Fail(error, "segment [0x%x, +0x%x) outside the local store", s.ls_start, s.size);
        if (s.type == SYS_SPU_SEGMENT_TYPE_COPY) {
            if (s.size && !m.Check(s.addr_or_value, s.size, vm::kPageReadable))
                return Fail(error, "COPY segment source 0x%08x+0x%x unreadable", s.addr_or_value, s.size);
            std::memcpy(ls + s.ls_start, m.shadow() + s.addr_or_value, s.size);
        } else if (s.type == SYS_SPU_SEGMENT_TYPE_FILL) {
            const u32 value = ByteSwap(s.addr_or_value);  // big-endian pattern in LS
            for (u32 off = 0; off < s.size; off += 4)
                std::memcpy(ls + s.ls_start + off, &value, std::min<u32>(4, s.size - off));
        } else {
            return Fail(error, "unknown SPU segment type %u", s.type);
        }
    }
    return true;
}

}  // namespace cell::spu
