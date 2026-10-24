// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — loader/prx_loader.h
//
// PS3 PRX modules (ELF64 big-endian, e_type 0xFFA4 ET_SCE_PPURELEXEC): system libraries in
// dev_flash/sys/external (libsre.sprx = cellSpurs...) and game-side .sprx files.
//
// PS4 -> PS3: bloodborne_pc never loads Orbis PRX code — it maps every libkernel/libSce*
// import to a host implementation (runtime_memory_resolve). That is what our HLE does too,
// but some PS3 libraries cannot be reimplemented faithfully (SPURS: its SPU half runs inside
// the game's own SPU task code), so they are loaded and executed for real (LLE).
//
// Layout of a PRX (as produced by the SDK and by RPCS3's decrypter):
//   PT_LOAD #0 (text, vaddr 0)      code, OPD-free read-only data, module info, lib tables
//   PT_LOAD #1 (data)               OPDs, TOC, import slots, writable data
//   0x700000A4 PT_SCE_PPURELA       relocation table, 24-byte big-endian entries:
//       u64 offset; u16 unk; u8 index_value; u8 index_addr; u32 type; u64 addend
//     site  = base(PT_LOAD[index_addr]) + offset
//     value = (index_value == 0xFF ? 0 : base(PT_LOAD[index_value])) + addend
//   module info: at base(PT_LOAD #0) + p_paddr - p_offset of PT_LOAD #0
//       u16 attributes; u8 version[2]; char name[28]; u32 toc;
//       u32 exports_start, exports_end;   libent table (28-byte entries)
//       u32 imports_start, imports_end;   libstub table (0x2C-byte entries, as in an EBOOT)
//   The export library with attribute 0x8000 holds the special entry points
//   (module_start 0xBC9A0086, module_stop 0xAB779874, module_exit 0x3AB9A95E,
//    module_prologue 0x0D10FD3F, module_epilogue 0x330F7005; variable module_info 0xD7F43016).
//
// Placement: segments keep their relative layout inside one block of the main area when they
// do not overlap (cross-segment references that escaped relocation stay valid); otherwise
// each gets its own block. Text becomes read-only and kPageCode after relocation.
#pragma once

#include <string>
#include <vector>

#include "hle/hle_registry.h"
#include "loader/ppu_linker.h"
#include "loader/ppu_loader.h"
#include "loader/self.h"
#include "memory/guest_memory.h"

namespace cell::loader {

inline constexpr u32 kNidModuleStart = 0xBC9A0086;
inline constexpr u32 kNidModuleStop = 0xAB779874;
inline constexpr u32 kNidModuleExit = 0x3AB9A95E;
inline constexpr u32 kNidModulePrologue = 0x0D10FD3F;
inline constexpr u32 kNidModuleEpilogue = 0x330F7005;
inline constexpr u32 kNidModuleInfo = 0xD7F43016;

// PowerPC64 ELF relocation types used by PRX modules.
inline constexpr u32 R_PPC64_NONE = 0;
inline constexpr u32 R_PPC64_ADDR32 = 1;
inline constexpr u32 R_PPC64_ADDR16_LO = 4;
inline constexpr u32 R_PPC64_ADDR16_HI = 5;
inline constexpr u32 R_PPC64_ADDR16_HA = 6;
inline constexpr u32 R_PPC64_REL24 = 10;
inline constexpr u32 R_PPC64_REL14 = 11;
inline constexpr u32 R_PPC64_ADDR64 = 38;
inline constexpr u32 R_PPC64_REL64 = 44;
inline constexpr u32 R_PPC64_ADDR16_LO_DS = 57;

struct ppu_prx_module_info_header_t {
    be_t<u16> attributes;
    u8 version[2];
    char name[28];
    be_t<u32> toc;
    be_t<u32> exports_start;
    be_t<u32> exports_end;
    be_t<u32> imports_start;
    be_t<u32> imports_end;
};
static_assert(sizeof(ppu_prx_module_info_header_t) == 52);

// One exported library ("libent").
struct ppu_prx_export_info_t {
    u8 size;  // 0x1C
    u8 unk0;
    be_t<u16> version;
    be_t<u16> attributes;  // 0x8000: the module's own special entries
    be_t<u16> num_func;
    be_t<u16> num_var;
    be_t<u16> num_tlsvar;
    u8 info_hash;
    u8 info_tlshash;
    u8 unk1[2];
    be_t<u32> name;   // char* (0 for the special library)
    be_t<u32> nids;   // u32[num_func + num_var]
    be_t<u32> addrs;  // OPD EAs for functions, then variable EAs
};
static_assert(sizeof(ppu_prx_export_info_t) == 28);

struct PrxSegment {
    u32 addr;    // where it was placed
    u32 vaddr;   // link-time address (relative)
    u32 filesz;
    u32 memsz;
    u32 flags;   // PF_*
};

struct PrxExportLibrary {
    std::string name;
    u16 version = 0;
    u16 attributes = 0;
    std::vector<std::pair<u32, u32>> functions;  // nid -> OPD
    std::vector<std::pair<u32, u32>> variables;  // nid -> EA (not linked yet)
};

struct PrxModule {
    std::string name;  // from module info, e.g. "cellSpurs_Library"
    std::string path;  // where it was loaded from (host path)
    u16 attributes = 0;
    u8 version[2] = {};
    u32 toc = 0;
    u32 module_start = 0, module_stop = 0, module_exit = 0;  // OPD EAs (0 = none)
    std::vector<PrxSegment> segments;
    std::vector<u32> blocks;  // allocation bases in the main area, freed on unload
    std::vector<PrxExportLibrary> exports;
    std::vector<ImportedModule> imports;
    u32 relocations = 0;
};

// One relocation: writes `value` (already base-adjusted) at `site`. Throws LoadError on an
// unsupported type or an out-of-range branch.
void ApplyPrxRelocation(vm::GuestMemory& memory, u32 type, u32 site, u64 value);

class PrxLoader {
public:
    PrxLoader(vm::GuestMemory& memory, PpuLinker& linker) noexcept : memory_(memory), linker_(linker) {}

    // Maps, relocates and links `exe` as module `id` (its lv2 object id). Imports are bound
    // (LLE first), exports registered, and every earlier importer relinked. Throws LoadError;
    // nothing stays allocated on failure.
    PrxModule Load(const PpuExecutable& exe, const std::string& path, u32 id);
    // Unlinks (importers fall back to HLE) and frees the module's memory.
    void Unload(PrxModule& module, u32 id);

private:
    void MapSegments(const PpuExecutable& exe, PrxModule& m);
    void Relocate(const PpuExecutable& exe, PrxModule& m);
    void Protect(const PrxModule& m);
    void ReadModuleInfo(const PpuExecutable& exe, PrxModule& m, u32 id);

    vm::GuestMemory& memory_;
    PpuLinker& linker_;
};

}  // namespace cell::loader
