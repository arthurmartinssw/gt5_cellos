// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — loader/ppu_loader.h
//
// Maps a parsed EBOOT into the guest arena and prepares the primary PPU thread.
//
// What it replaces in bloodborne_pc (src/probe.c, main path):
//
//   probe.c                                              ppu_loader.cpp
//   ---------------------------------------------------  ------------------------------------------------
//   image = allocate(round_page(size)); fread(image)       PT_LOAD segments placed at their *fixed* vaddr
//     — one host blob, base chosen by the host              (ET_EXEC, usually 0x10000) in Area::Main;
//                                                            BSS is zero from the fresh arena pages
//   relocations (kind 0/1/2) + native bindings            none for ET_EXEC: the EBOOT is linked at its
//     into the GOT                                          address. Imports are libstub tables reached
//                                                            via PT 0x60000002, patched with HLE OPDs
//   procparam (sdk_version at +16)                        sys_process_param_t via PT 0x60000001: SDK
//                                                            version, primary thread prio/stack size,
//                                                            malloc page size
//   main_tls[] / TLS module metadata                      PT_TLS image copied into a TLS block,
//                                                            r13 = block + 0x30 + 0x7000
//   patch_guest_tls: fs:[0] -> gs/TEB rewrite             nothing to patch: TLS is r13, a guest register
//   runtime_low_map(8 MiB) stack; enter_on_stack(entry,   stack in Area::Stack (size from the process
//     &params, guest_exit, top)                            param) with a guard; e_entry is an OPD
//                                                            {code, toc}; registers r1..r13 set in a
//                                                            PpuThreadContext handed to the JIT
#pragma once

#include <memory>

#include <string>
#include <vector>

#include "common/types.h"
#include "hle/hle_registry.h"
#include "loader/self.h"
#include "memory/guest_memory.h"
#include "ppu/ppu_context.h"

namespace cell::loader {

inline constexpr u32 kProcParamMagic = 0x13BCC5F6;
inline constexpr u32 kPrxParamMagic = 0x1B434CEC;
inline constexpr u32 kPrimaryThreadId = 0x01000000;  // lv2 PPU thread ids start here
inline constexpr u32 kTlsTcbSize = 0x30;              // lv2 header before the TLS image
inline constexpr u32 kTlsTpOffset = 0x7000;           // PPC64 ELF ABI: r13 = TLS start + 0x7000
inline constexpr u32 kStackFrameHeader = 0x70;        // PPC64 minimum frame (back chain, CR, LR, ...)

// ---- guest structures (exact PPU layouts) ----
struct sys_process_param_t {
    be_t<u32> size;
    be_t<u32> magic;  // kProcParamMagic
    be_t<u32> version;
    be_t<u32> sdk_version;
    be_t<s32> primary_prio;
    be_t<u32> primary_stacksize;
    be_t<u32> malloc_pagesize;
    be_t<u32> ppc_seg;
    // newer SDKs append crash_dump_param_addr; `size` tells how much is present
};
static_assert(sizeof(sys_process_param_t) == 0x20);

struct sys_process_prx_info_t {
    be_t<u32> size;
    be_t<u32> magic;  // kPrxParamMagic
    be_t<u32> version;
    be_t<u32> unk0;
    be_t<u32> libent_start;  // exported libraries (the EBOOT exports little or nothing)
    be_t<u32> libent_end;
    be_t<u32> libstub_start;  // imported libraries: array of ppu_prx_module_info_t
    be_t<u32> libstub_end;
};
static_assert(sizeof(sys_process_prx_info_t) == 0x20);

// One imported library ("libstub"). The FNID array and the slot array run in parallel:
// slot[i] receives the address of the OPD to call for nids[i].
struct ppu_prx_module_info_t {
    u8 size;  // 0x2C
    u8 unk0;
    be_t<u16> version;
    be_t<u16> attributes;
    be_t<u16> num_func;
    be_t<u16> num_var;
    be_t<u16> num_tlsvar;
    u8 info_hash;
    u8 info_tlshash;
    u8 unk1[2];
    be_t<u32> name;    // char* module name
    be_t<u32> nids;    // u32[num_func] FNIDs
    be_t<u32> addrs;   // u32[num_func] import slots (OPD pointers)
    be_t<u32> vnids;   // u32[num_var] variable NIDs
    be_t<u32> vstubs;  // variable stub table
    be_t<u32> unk4;
    be_t<u32> unk5;
};
static_assert(sizeof(ppu_prx_module_info_t) == 0x2C);

// ---- loader output ----
struct ProcessParam {
    bool present = false;
    u32 sdk_version = 0;
    s32 primary_prio = 1001;
    u32 primary_stacksize = 0x100000;
    u32 malloc_pagesize = 0x100000;
    u32 ppc_seg = 0;
};

struct TlsImage {
    u32 vaddr = 0;
    u32 filesz = 0;
    u32 memsz = 0;
    u32 align = 0;
};

struct ImportedFunction {
    u32 nid;
    u32 slot;          // EA of the patched pointer
    u32 hle_index;     // HleRegistry index (when bound to HLE)
    u32 lle_addr = 0;  // OPD of a loaded PRX export (when bound to LLE), else 0
};

struct ImportedModule {
    std::string name;
    u16 version = 0;
    u16 attributes = 0;
    u32 variable_count = 0;  // resolved in a later phase (HLE variables)
    std::vector<ImportedFunction> functions;
};

struct LoadedSegment {
    u32 vaddr;
    u32 filesz;
    u32 memsz;
    u32 flags;  // PF_R / PF_W / PF_X
};

struct PpuBootImage {
    u32 entry_opd = 0;
    u32 entry_pc = 0;
    u32 entry_toc = 0;
    u32 image_low = 0;   // lowest mapped byte of the executable
    u32 image_high = 0;  // one past the highest
    ProcessParam proc;
    TlsImage tls;
    u32 tls_block = 0;
    u32 tls_pointer = 0;  // initial r13
    u32 stack_base = 0;   // includes the 64 KiB guard at the bottom
    u32 stack_size = 0;
    u32 initial_sp = 0;
    u32 argc = 0;
    u32 argv = 0;  // EA of argv[]: 64-bit elements, as lv2 passes them (the crt0 narrows them)
    u32 envp = 0;  // EA of envp[] (64-bit elements), right after argv[argc] = NULL
    std::vector<LoadedSegment> segments;
    std::vector<ImportedModule> imports;
};

struct LoaderOptions {
    // argv[0] the game sees; disc titles run from the Blu-ray mount.
    std::string argv0 = "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN";
    std::vector<std::string> args;
};

class PpuLinker;

class PpuLoader {
public:
    // `linker` resolves imports (LLE PRX exports first, HLE otherwise); without one the loader
    // uses a private linker, i.e. HLE only.
    PpuLoader(vm::GuestMemory& memory, hle::HleRegistry& hle, PpuLinker* linker = nullptr);
    ~PpuLoader();

    // Throws LoadError. On success every import slot points at an HLE OPD.
    PpuBootImage Load(const PpuExecutable& exe, const LoaderOptions& options);

    // Initial register state of the primary PPU thread.
    void InitializePrimaryThread(const PpuBootImage& image, ppu::PpuThreadContext& ctx) const;

private:
    void MapSegments(const PpuExecutable& exe, PpuBootImage& image);
    void ReadProcessParam(const PpuExecutable& exe, PpuBootImage& image);
    void BindImports(const PpuExecutable& exe, PpuBootImage& image);
    void SetupTls(const PpuExecutable& exe, PpuBootImage& image);
    void ResolveEntry(const PpuExecutable& exe, PpuBootImage& image);
    void SetupStack(const LoaderOptions& options, PpuBootImage& image);
    [[nodiscard]] bool InSegment(const PpuBootImage& image, u32 addr, u32 size, u32 need_flags) const noexcept;

    vm::GuestMemory& memory_;
    hle::HleRegistry& hle_;
    std::unique_ptr<PpuLinker> own_linker_;
    PpuLinker* linker_;
};

}  // namespace cell::loader
