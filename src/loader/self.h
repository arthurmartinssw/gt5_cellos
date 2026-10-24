// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — loader/self.h
//
// Parser for PS3 PPU executables: a plain big-endian ELF64 (EM_PPC64), or a SELF wrapper
// ("SCE\0") around one. C++ counterpart of bloodborne_pc/scripts/prepare.py (parse_self /
// parse_elf), with the PS4 -> PS3 differences:
//
//   prepare.py (PS4)                                   self.cpp (PS3)
//   -------------------------------------------------  ---------------------------------------------
//   magic 4F 15 3D 1D, little-endian header             magic "SCE\0" (0x53434500), big-endian
//   segment table at 0x20, 32 bytes per entry:          extended header (0x50 bytes) with offsets to
//     flags bit 0x800 = has data, bits 20.. = phdr idx   app info, ELF header, phdrs and a section-info
//                                                        table: one 0x20 entry per phdr, in order
//   ELF: '\x7fELF\x02\x01\x01', e_machine 62 (x86-64)    '\x7fELF\x02\x02\x01', OSABI 0x66 (CELL LV2),
//                                                        e_machine 21 (PPC64)
//   PT_LOAD 1, PT_DYNAMIC 2, 0x61000000 (SCE dynlib)     PT_LOAD 1, PT_TLS 7, 0x60000001 (process
//                                                        param), 0x60000002 (PRX param: lib tables)
//   rejects flags & 2 (encrypted) or & 8 (compressed)    zlib-compressed segments are inflated;
//                                                        encrypted (retail) SELFs are rejected
//   "No decryption" policy                              same policy: no keys in this code base.
//                                                        Retail EBOOT.BIN must be decrypted from the
//                                                        user's own dump beforehand (debug/fake-signed
//                                                        SELFs with key revision 0x8000 load as-is).
#pragma once

#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/types.h"

namespace cell::loader {

inline constexpr u32 PT_LOAD = 1;
inline constexpr u32 PT_TLS = 7;
inline constexpr u32 PT_PROC_PARAM = 0x60000001;
inline constexpr u32 PT_PROC_PRX_PARAM = 0x60000002;
inline constexpr u32 PT_SCE_PPURELA = 0x700000A4;  // PRX relocation table (24-byte entries)
inline constexpr u32 PF_X = 1, PF_W = 2, PF_R = 4;

inline constexpr u16 ET_EXEC = 2;
inline constexpr u16 ET_SCE_PPURELEXEC = 0xFFA4;  // PRX (.sprx / .prx): relocatable PPU module
inline constexpr u16 EM_PPC64 = 21;
inline constexpr u8 ELFOSABI_CELL_LV2 = 0x66;

inline constexpr u32 kSceMagic = 0x53434500;     // "SCE\0"
inline constexpr u16 kSceHeaderTypeSelf = 1;
inline constexpr u16 kKeyRevisionDebug = 0x8000; // fake-signed / debug: plaintext payload

class LoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ElfProgramHeader {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
};

struct SelfInfo {
    bool wrapped = false;  // false: the file was a plain ELF
    u16 key_revision = 0;
    u32 self_type = 0;     // app info self_type (4 = application, 8 = NPDRM)
    u64 auth_id = 0;
    u64 app_version = 0;
};

// Owns the file bytes and any inflated segment buffers; payload spans point into them, so
// the object is move-only (moves keep heap buffers in place).
class PpuExecutable {
public:
    PpuExecutable() = default;
    PpuExecutable(const PpuExecutable&) = delete;
    PpuExecutable& operator=(const PpuExecutable&) = delete;
    PpuExecutable(PpuExecutable&&) noexcept = default;
    PpuExecutable& operator=(PpuExecutable&&) noexcept = default;

    SelfInfo self;
    u16 type = 0;
    u8 osabi = 0;
    u64 entry = 0;  // EA of the entry OPD, not of code
    std::vector<ElfProgramHeader> phdrs;
    std::vector<std::span<const u8>> payload;  // per phdr: exactly filesz bytes (or empty)

    std::vector<u8> file;
    std::vector<std::vector<u8>> inflated;
};

[[nodiscard]] std::vector<u8> ReadWholeFile(const std::filesystem::path& path);
// Throws LoadError with a precise reason (offset, field) on anything malformed.
// The main executable (ET_EXEC, EBOOT.BIN):
[[nodiscard]] PpuExecutable ParseExecutable(std::vector<u8> file);
// A PRX module (ET_SCE_PPURELEXEC 0xFFA4), e.g. dev_flash/sys/external/libsre.sprx decrypted:
[[nodiscard]] PpuExecutable ParsePrx(std::vector<u8> file);

}  // namespace cell::loader
