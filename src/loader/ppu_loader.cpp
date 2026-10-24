// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — loader/ppu_loader.cpp
#include "loader/ppu_loader.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <map>

#include "common/log.h"
#include "loader/ppu_linker.h"

namespace cell::loader {

namespace {

template <typename... Args>
[[noreturn]] void Fail(const char* format, Args... args) {
    char message[512];
    if constexpr (sizeof...(Args) == 0) std::snprintf(message, sizeof(message), "%s", format);
    else std::snprintf(message, sizeof(message), format, args...);
    throw LoadError(message);
}

const ElfProgramHeader* FindPhdr(const PpuExecutable& exe, u32 type, std::size_t* index = nullptr) {
    for (std::size_t i = 0; i < exe.phdrs.size(); ++i) {
        if (exe.phdrs[i].type == type) {
            if (index) *index = i;
            return &exe.phdrs[i];
        }
    }
    return nullptr;
}

}  // namespace

PpuLoader::PpuLoader(vm::GuestMemory& memory, hle::HleRegistry& hle, PpuLinker* linker)
    : memory_(memory), hle_(hle), own_linker_(linker ? nullptr : std::make_unique<PpuLinker>(memory, hle)),
      linker_(linker ? linker : own_linker_.get()) {}

PpuLoader::~PpuLoader() = default;

bool PpuLoader::InSegment(const PpuBootImage& image, u32 addr, u32 size, u32 need_flags) const noexcept {
    for (const LoadedSegment& s : image.segments) {
        if (addr >= s.vaddr && u64{addr} + size <= u64{s.vaddr} + s.memsz && (s.flags & need_flags) == need_flags)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// 1. PT_LOAD -> Area::Main at the linked address.
// ---------------------------------------------------------------------------------------
void PpuLoader::MapSegments(const PpuExecutable& exe, PpuBootImage& image) {
    const vm::AreaDesc& main = vm::Describe(vm::Area::Main);
    struct Range {
        u64 lo, hi;
    };
    std::vector<Range> ranges;

    for (std::size_t i = 0; i < exe.phdrs.size(); ++i) {
        const ElfProgramHeader& p = exe.phdrs[i];
        if (p.type != PT_LOAD || !p.memsz) continue;
        if (p.filesz && exe.payload[i].size() != p.filesz) Fail("PT_LOAD %zu has no payload in this file", i);
        // lv2 maps executable segments with 64 KiB pages; PS3 linkers align PT_LOAD to 0x10000.
        const u64 lo = AlignDown<u64>(p.vaddr, vm::k64K);
        const u64 hi = AlignUp<u64>(p.vaddr + p.memsz, vm::k64K);
        if (lo < main.base || hi > u64{main.base} + main.size)
            Fail("PT_LOAD %zu [0x%" PRIx64 ", 0x%" PRIx64 ") is outside the main area", i, p.vaddr, p.vaddr + p.memsz);
        ranges.push_back({lo, hi});
        image.segments.push_back({static_cast<u32>(p.vaddr), static_cast<u32>(p.filesz),
                                  static_cast<u32>(p.memsz), p.flags & (PF_R | PF_W | PF_X)});
    }
    if (image.segments.empty()) Fail("executable has no PT_LOAD segments");

    // Text and data may share a 64 KiB block when a linker packs them: merge before reserving.
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.lo < b.lo; });
    std::vector<Range> merged;
    for (const Range& r : ranges) {
        if (!merged.empty() && r.lo <= merged.back().hi) merged.back().hi = std::max(merged.back().hi, r.hi);
        else merged.push_back(r);
    }
    // Commit RW first: the copy goes through the shadow view anyway, final protection below.
    for (const Range& r : merged) {
        if (!memory_.AllocateFixed(vm::Area::Main, static_cast<u32>(r.lo), r.hi - r.lo, vm::kProtReadWrite))
            Fail("cannot reserve [0x%08" PRIx64 ", 0x%08" PRIx64 ") in the main area", r.lo, r.hi);
    }
    image.image_low = static_cast<u32>(merged.front().lo);
    image.image_high = static_cast<u32>(merged.back().hi);

    // Copy file bytes. BSS (memsz - filesz) is already zero: fresh arena pages read as zero
    // (sparse memfd on Linux, newly committed section pages on Windows).
    for (std::size_t i = 0; i < exe.phdrs.size(); ++i) {
        const ElfProgramHeader& p = exe.phdrs[i];
        if (p.type == PT_LOAD && p.filesz)
            memory_.CopyIn(static_cast<u32>(p.vaddr), exe.payload[i].data(), exe.payload[i].size());
    }

    // Final per-4K protection: union of the segments touching each page. Text becomes
    // read-only and is flagged kPageCode (the JIT translates it and watches it for writes);
    // the guest view never needs host-executable pages because PPU code is never run natively.
    std::map<u32, u8> page_prot;
    for (const LoadedSegment& s : image.segments) {
        const u8 prot = (s.flags & PF_W) ? vm::kProtReadWrite : vm::kProtRead;
        const u32 first = AlignDown<u32>(s.vaddr, vm::kPageSize);
        const u64 last = AlignUp<u64>(u64{s.vaddr} + s.memsz, vm::kPageSize);
        for (u64 page = first; page < last; page += vm::kPageSize) page_prot[static_cast<u32>(page)] |= prot;
        if (s.flags & PF_X) memory_.MarkCode(s.vaddr, s.memsz);
    }
    // Pages of the reserved 64 KiB blocks not covered by any segment stay RW (lv2 maps whole
    // 64 KiB pages; being stricter would only create false faults).
    for (auto it = page_prot.begin(); it != page_prot.end();) {
        const u32 start = it->first;
        const u8 prot = it->second;
        u32 end = start + vm::kPageSize;
        auto next = std::next(it);
        while (next != page_prot.end() && next->first == end && next->second == prot) {
            end += vm::kPageSize;
            ++next;
        }
        if (prot != vm::kProtReadWrite && !memory_.Protect(start, end - start, prot))
            Fail("cannot protect [0x%08x, 0x%08x)", start, end);
        it = next;
    }
}

// ---------------------------------------------------------------------------------------
// 2. sys_process_param_t (PT 0x60000001) — the SYS_PROCESS_PARAM() block of the game.
// ---------------------------------------------------------------------------------------
void PpuLoader::ReadProcessParam(const PpuExecutable& exe, PpuBootImage& image) {
    const ElfProgramHeader* p = FindPhdr(exe, PT_PROC_PARAM);
    if (!p || !p->filesz) {
        LOG_WARN("loader", "no process param segment; using lv2 defaults (prio 1001, 1 MiB stack)");
        return;
    }
    const u32 at = static_cast<u32>(p->vaddr);
    if (p->filesz < sizeof(sys_process_param_t) || !memory_.Check(at, sizeof(sys_process_param_t), vm::kPageReadable))
        Fail("process param at 0x%08x is truncated or unmapped", at);
    const auto param = memory_.ReadStruct<sys_process_param_t>(at);
    if (param.magic != kProcParamMagic) Fail("process param magic 0x%08x (expected 0x13bcc5f6)", param.magic.get());

    ProcessParam& out = image.proc;
    out.present = true;
    out.sdk_version = param.sdk_version;
    out.primary_prio = param.primary_prio;
    out.primary_stacksize = param.primary_stacksize ? param.primary_stacksize.get() : 0x100000u;
    out.malloc_pagesize = param.malloc_pagesize;
    out.ppc_seg = param.ppc_seg;
    if (out.malloc_pagesize != vm::k1M && out.malloc_pagesize != vm::k64K) {
        LOG_WARN("loader", "malloc_pagesize 0x%x is neither 64K nor 1M; using 1M", out.malloc_pagesize);
        out.malloc_pagesize = vm::k1M;
    }
}

// ---------------------------------------------------------------------------------------
// 3. TLS: PT_TLS image -> per-thread block. r13 = block + 0x30 (lv2 TCB) + 0x7000 (ABI bias).
// ---------------------------------------------------------------------------------------
void PpuLoader::SetupTls(const PpuExecutable& exe, PpuBootImage& image) {
    if (const ElfProgramHeader* p = FindPhdr(exe, PT_TLS); p && p->memsz) {
        image.tls = {static_cast<u32>(p->vaddr), static_cast<u32>(p->filesz), static_cast<u32>(p->memsz),
                     static_cast<u32>(p->align)};
        if (image.tls.filesz && !memory_.Check(image.tls.vaddr, image.tls.filesz, vm::kPageReadable))
            Fail("TLS initialization image at 0x%08x is not inside a loaded segment", image.tls.vaddr);
    }
    const u32 block_size = kTlsTcbSize + AlignUp<u32>(image.tls.memsz, 16);
    const auto block = memory_.Allocate(vm::Area::Main, block_size, vm::k64K, vm::kProtReadWrite);
    if (!block) Fail("cannot allocate the primary thread TLS block (0x%x bytes)", block_size);
    if (image.tls.filesz)
        std::memcpy(memory_.shadow() + *block + kTlsTcbSize, memory_.shadow() + image.tls.vaddr, image.tls.filesz);
    // .tbss (memsz - filesz) is already zero.
    image.tls_block = *block;
    image.tls_pointer = *block + kTlsTcbSize + kTlsTpOffset;
}

// ---------------------------------------------------------------------------------------
// 4. Imports: walk the libstub tables, bind every FNID to an HLE thunk, patch the slots.
// ---------------------------------------------------------------------------------------
void PpuLoader::BindImports(const PpuExecutable& exe, PpuBootImage& image) {
    const ElfProgramHeader* p = FindPhdr(exe, PT_PROC_PRX_PARAM);
    if (!p || !p->filesz) {
        LOG_WARN("loader", "no PRX param segment: the executable imports nothing");
        return;
    }
    const u32 at = static_cast<u32>(p->vaddr);
    if (!memory_.Check(at, sizeof(sys_process_prx_info_t), vm::kPageReadable))
        Fail("PRX param at 0x%08x is unmapped", at);
    const auto info = memory_.ReadStruct<sys_process_prx_info_t>(at);
    if (info.magic != kPrxParamMagic) Fail("PRX param magic 0x%08x (expected 0x1b434cec)", info.magic.get());

    image.imports = linker_->BindImportTable(info.libstub_start, info.libstub_end, PpuLinker::kMainExecutable);
    std::size_t total = 0, implemented = 0, lle = 0;
    for (const ImportedModule& m : image.imports) {
        for (const ImportedFunction& f : m.functions) {
            ++total;
            if (f.lle_addr) ++lle;
            else implemented += hle_.At(f.hle_index).implemented;
        }
    }
    LOG_INFO("loader", "imports: %zu modules, %zu functions (%zu LLE, %zu HLE with host implementations)",
             image.imports.size(), total, lle, implemented);
}

// ---------------------------------------------------------------------------------------
// 5. Entry: e_entry is the EA of an OPD {u32 code, u32 toc} (32-bit pointers on PS3).
// ---------------------------------------------------------------------------------------
void PpuLoader::ResolveEntry(const PpuExecutable& exe, PpuBootImage& image) {
    if (exe.entry > 0xFFFFFFF8ull) Fail("e_entry 0x%" PRIx64 " is not a 32-bit EA", exe.entry);
    image.entry_opd = static_cast<u32>(exe.entry);
    if (!InSegment(image, image.entry_opd, 8, 0))
        Fail("entry OPD 0x%08x is outside the loaded segments", image.entry_opd);
    image.entry_pc = memory_.Read<u32>(image.entry_opd);
    image.entry_toc = memory_.Read<u32>(image.entry_opd + 4);
    if ((image.entry_pc & 3) || !InSegment(image, image.entry_pc, 4, PF_X))
        Fail("entry code 0x%08x (from OPD 0x%08x) is not in an executable segment", image.entry_pc, image.entry_opd);
}

// ---------------------------------------------------------------------------------------
// 6. Primary thread stack + argv/envp, laid out like lv2 hands them to the crt0.
// ---------------------------------------------------------------------------------------
void PpuLoader::SetupStack(const LoaderOptions& options, PpuBootImage& image) {
    const u32 size = AlignUp<u32>(std::max<u32>(image.proc.primary_stacksize, vm::k64K), vm::k64K);
    const u32 total = size + vm::k64K;  // + guard below the stack
    const auto base = memory_.Allocate(vm::Area::Stack, total, vm::k64K, vm::kProtReadWrite);
    if (!base) Fail("cannot allocate the primary thread stack (0x%x bytes)", total);
    memory_.Protect(*base, vm::k64K, vm::kProtNone);  // overflow faults instead of corrupting
    image.stack_base = *base;
    image.stack_size = total;

    std::vector<std::string> args{options.argv0};
    args.insert(args.end(), options.args.begin(), options.args.end());

    // lv2 hands the crt0 argv[] and envp[] as arrays of **64-bit** elements (each holding a
    // 32-bit EA, upper word 0), envp[] directly after argv[]'s terminator — the layout RPCS3's
    // ppu_load_exec builds with vm::ptr<u64>. The crt0 then narrows them *in place* to the
    // 32-bit char* arrays that ILP32 PS3 code uses (ld from slot i*8, stw to slot i*4) before
    // storing argc/argv for main. Handing it 32-bit elements made that narrowing read
    // {argv[0], argv[1]} as one doubleword and store its low word (argv[1] = NULL) into
    // argv[0]: "MGS4 MAIN" then dereferenced argv[0] = 0 at pc 0x10710 (lwz r9,0(r29); lbz).
    u32 sp = *base + total;
    std::vector<u32> pointers;
    for (const std::string& arg : args) {
        sp -= static_cast<u32>(arg.size() + 1);
        memory_.CopyIn(sp, arg.c_str(), arg.size() + 1);
        pointers.push_back(sp);
    }
    sp = AlignDown<u32>(sp, 16);
    const u32 slots = static_cast<u32>(pointers.size()) + 1 + 1;  // argv[argc] = NULL, envp[0] = NULL
    sp = AlignDown<u32>(sp - slots * 8, 16);
    image.argv = sp;
    for (std::size_t i = 0; i < pointers.size(); ++i) memory_.Write<u64>(sp + static_cast<u32>(i * 8), pointers[i]);
    memory_.Write<u64>(sp + static_cast<u32>(pointers.size() * 8), 0);
    image.envp = sp + static_cast<u32>((pointers.size() + 1) * 8);
    memory_.Write<u64>(image.envp, 0);

    // First frame: 16-byte aligned, back chain = 0 terminates guest stack walks.
    sp = AlignDown<u32>(sp, 16) - kStackFrameHeader;
    memory_.Write<u64>(sp, 0);
    image.initial_sp = sp;
    image.argc = static_cast<u32>(args.size());
}

PpuBootImage PpuLoader::Load(const PpuExecutable& exe, const LoaderOptions& options) {
    PpuBootImage image;
    MapSegments(exe, image);
    ReadProcessParam(exe, image);
    SetupTls(exe, image);
    BindImports(exe, image);
    hle_.Materialize();  // also places internal thunks (process exit) when nothing was imported
    ResolveEntry(exe, image);
    SetupStack(options, image);

    LOG_INFO("loader", "image [0x%08x, 0x%08x), entry OPD 0x%08x -> pc 0x%08x toc 0x%08x", image.image_low,
             image.image_high, image.entry_opd, image.entry_pc, image.entry_toc);
    LOG_INFO("loader", "sdk 0x%08x, primary prio %d, stack 0x%x @ 0x%08x, malloc pages 0x%x, TLS 0x%x/0x%x",
             image.proc.sdk_version, image.proc.primary_prio, image.proc.primary_stacksize, image.stack_base,
             image.proc.malloc_pagesize, image.tls.filesz, image.tls.memsz);
    return image;
}

void PpuLoader::InitializePrimaryThread(const PpuBootImage& image, ppu::PpuThreadContext& ctx) const {
    ctx = ppu::PpuThreadContext{};
    // Register convention for the crt0 _start, as RPCS3's ppu_load_exec passes it:
    //   r3 argc, r4 argv, r5 envp, r6 0, r7 thread id, r8-r10 TLS image (vaddr, filesz, memsz),
    //   r11 e_entry (the OPD), r12 malloc page size.
    ctx.gpr[1] = image.initial_sp;
    ctx.gpr[2] = image.entry_toc;
    ctx.gpr[3] = image.argc;
    ctx.gpr[4] = image.argv;
    ctx.gpr[5] = image.envp;
    ctx.gpr[6] = 0;
    ctx.gpr[7] = kPrimaryThreadId;
    ctx.gpr[8] = image.tls.vaddr;
    ctx.gpr[9] = image.tls.filesz;
    ctx.gpr[10] = image.tls.memsz;
    ctx.gpr[11] = image.entry_opd;
    ctx.gpr[12] = image.proc.malloc_pagesize;
    // Pre-built TLS block for the main thread; the crt0 may build its own from r8-r10.
    ctx.gpr[13] = image.tls_pointer;
    // Returning from the entry function falls into the process-exit HLE thunk.
    ctx.lr = hle_.CodeAddress(hle_.exit_index());
    ctx.cia = image.entry_pc;
    // Cell VMX runs with VSCR[NJ] (non-Java mode) set: denormals flush to zero.
    ctx.vscr = 0x00010000;
    ctx.thread_id = kPrimaryThreadId;
    ctx.stack_addr = image.stack_base;
    ctx.stack_size = image.stack_size;
}

}  // namespace cell::loader
