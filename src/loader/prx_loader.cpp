// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — loader/prx_loader.cpp
#include "loader/prx_loader.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>

#include "common/log.h"

namespace cell::loader {

namespace {

template <typename... Args>
[[noreturn]] void Fail(const char* format, Args... args) {
    char message[512];
    if constexpr (sizeof...(Args) == 0) std::snprintf(message, sizeof(message), "%s", format);
    else std::snprintf(message, sizeof(message), format, args...);
    throw LoadError(message);
}

u64 Be64(const u8* p) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
    return v;
}
u32 Be32(const u8* p) { return u32{p[0]} << 24 | u32{p[1]} << 16 | u32{p[2]} << 8 | p[3]; }

const char* SpecialName(u32 nid) {
    switch (nid) {
    case kNidModuleStart: return "module_start";
    case kNidModuleStop: return "module_stop";
    case kNidModuleExit: return "module_exit";
    case kNidModulePrologue: return "module_prologue";
    case kNidModuleEpilogue: return "module_epilogue";
    case kNidModuleInfo: return "module_info";
    default: return nullptr;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Relocations
// ---------------------------------------------------------------------------------------

void ApplyPrxRelocation(vm::GuestMemory& m, u32 type, u32 site, u64 value) {
    const u32 v32 = static_cast<u32>(value);
    switch (type) {
    case R_PPC64_NONE: return;
    case R_PPC64_ADDR32: m.Write<u32>(site, v32); return;
    case R_PPC64_ADDR16_LO: m.Write<u16>(site, static_cast<u16>(v32)); return;
    case R_PPC64_ADDR16_HI: m.Write<u16>(site, static_cast<u16>(v32 >> 16)); return;
    case R_PPC64_ADDR16_HA: m.Write<u16>(site, static_cast<u16>((v32 + 0x8000) >> 16)); return;
    case R_PPC64_ADDR16_LO_DS: m.Write<u16>(site, static_cast<u16>((m.Read<u16>(site) & 3) | (v32 & 0xFFFC))); return;
    case R_PPC64_REL24: {
        const s64 delta = static_cast<s64>(v32) - static_cast<s64>(site);
        if (delta < -0x2000000 || delta >= 0x2000000 || (delta & 3))
            Fail("REL24 at 0x%08x: target 0x%08x out of branch range", site, v32);
        m.Write<u32>(site, (m.Read<u32>(site) & 0xFC000003u) | (static_cast<u32>(delta) & 0x03FFFFFCu));
        return;
    }
    case R_PPC64_REL14: {
        const s64 delta = static_cast<s64>(v32) - static_cast<s64>(site);
        if (delta < -0x8000 || delta >= 0x8000 || (delta & 3))
            Fail("REL14 at 0x%08x: target 0x%08x out of branch range", site, v32);
        m.Write<u32>(site, (m.Read<u32>(site) & 0xFFFF0003u) | (static_cast<u32>(delta) & 0xFFFCu));
        return;
    }
    case R_PPC64_ADDR64: m.Write<u64>(site, value); return;
    case R_PPC64_REL64: m.Write<u64>(site, value - site); return;
    default: Fail("unsupported relocation type %u at 0x%08x", type, site);
    }
}

// ---------------------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------------------

void PrxLoader::MapSegments(const PpuExecutable& exe, PrxModule& m) {
    struct Load {
        std::size_t phdr;
        u64 vaddr, filesz, memsz;
        u32 flags;
    };
    std::vector<Load> loads;
    for (std::size_t i = 0; i < exe.phdrs.size(); ++i) {
        const ElfProgramHeader& p = exe.phdrs[i];
        if (p.type != PT_LOAD || !p.memsz) continue;
        if (p.filesz && exe.payload[i].size() != p.filesz) Fail("PT_LOAD %zu has no payload in this file", i);
        if (p.memsz > 0x10000000ull || p.vaddr > 0x10000000ull) Fail("PT_LOAD %zu is implausibly large", i);
        loads.push_back({i, p.vaddr, p.filesz, p.memsz, p.flags});
    }
    if (loads.empty()) Fail("PRX has no PT_LOAD segments");

    // Contiguous placement when the link-time ranges do not overlap.
    auto sorted = loads;
    std::sort(sorted.begin(), sorted.end(), [](const Load& a, const Load& b) { return a.vaddr < b.vaddr; });
    bool overlap = false;
    for (std::size_t i = 1; i < sorted.size(); ++i)
        overlap |= sorted[i].vaddr < sorted[i - 1].vaddr + sorted[i - 1].memsz;

    auto allocate = [&](u64 size) -> u32 {
        const auto base = memory_.Allocate(vm::Area::Main, AlignUp<u64>(size, vm::k64K), vm::k64K, vm::kProtReadWrite);
        if (!base) Fail("out of main-area memory for 0x%" PRIx64 " bytes of PRX segments", size);
        m.blocks.push_back(*base);
        return *base;
    };
    if (!overlap) {
        const u64 lo = AlignDown<u64>(sorted.front().vaddr, vm::k64K);
        const u64 hi = sorted.back().vaddr + sorted.back().memsz;
        const u32 base = allocate(hi - lo);
        for (const Load& l : loads)
            m.segments.push_back({static_cast<u32>(base + (l.vaddr - lo)), static_cast<u32>(l.vaddr),
                                  static_cast<u32>(l.filesz), static_cast<u32>(l.memsz), l.flags});
    } else {
        for (const Load& l : loads)
            m.segments.push_back({allocate(l.memsz), static_cast<u32>(l.vaddr), static_cast<u32>(l.filesz),
                                  static_cast<u32>(l.memsz), l.flags});
    }
    for (std::size_t k = 0; k < loads.size(); ++k)
        if (loads[k].filesz) memory_.CopyIn(m.segments[k].addr, exe.payload[loads[k].phdr].data(), loads[k].filesz);
}

void PrxLoader::Relocate(const PpuExecutable& exe, PrxModule& m) {
    for (std::size_t i = 0; i < exe.phdrs.size(); ++i) {
        if (exe.phdrs[i].type != PT_SCE_PPURELA || !exe.phdrs[i].filesz) continue;
        const auto& table = exe.payload[i];
        if (table.size() % 24) Fail("relocation table %zu: size 0x%zx is not a multiple of 24", i, table.size());
        for (std::size_t at = 0; at < table.size(); at += 24) {
            const u8* e = table.data() + at;
            const u64 offset = Be64(e + 0);
            const u8 index_value = e[10], index_addr = e[11];
            const u32 type = Be32(e + 12);
            const u64 addend = Be64(e + 16);
            if (index_addr >= m.segments.size()) Fail("relocation %zu: site segment %u does not exist", at / 24, index_addr);
            const PrxSegment& site_seg = m.segments[index_addr];
            if (offset + 2 > site_seg.memsz) Fail("relocation %zu: offset 0x%" PRIx64 " outside segment %u", at / 24, offset, index_addr);
            u64 value = addend;
            if (index_value != 0xFF) {
                if (index_value >= m.segments.size()) Fail("relocation %zu: value segment %u does not exist", at / 24, index_value);
                value += m.segments[index_value].addr;
            }
            ApplyPrxRelocation(memory_, type, static_cast<u32>(site_seg.addr + offset), value);
            ++m.relocations;
        }
    }
}

void PrxLoader::Protect(const PrxModule& m) {
    // Union of the protections of the segments touching each 4 KiB page (as for the EBOOT).
    std::map<u32, u8> pages;
    for (const PrxSegment& s : m.segments) {
        const u8 prot = (s.flags & PF_W) ? vm::kProtReadWrite : vm::kProtRead;
        for (u64 p = AlignDown<u32>(s.addr, vm::kPageSize); p < AlignUp<u64>(u64{s.addr} + s.memsz, vm::kPageSize); p += vm::kPageSize)
            pages[static_cast<u32>(p)] |= prot;
        if (s.flags & PF_X) memory_.MarkCode(s.addr, s.memsz);
    }
    for (const auto& [page, prot] : pages)
        if (prot != vm::kProtReadWrite) memory_.Protect(page, vm::kPageSize, prot);
}

void PrxLoader::ReadModuleInfo(const PpuExecutable& exe, PrxModule& m, u32 id) {
    const ElfProgramHeader* first = nullptr;
    for (const ElfProgramHeader& p : exe.phdrs)
        if (p.type == PT_LOAD && p.memsz) {
            first = &p;
            break;
        }
    const PrxSegment& seg0 = m.segments.front();
    const u64 rel = first->paddr - first->offset;
    if (first->paddr < first->offset || rel + sizeof(ppu_prx_module_info_header_t) > seg0.memsz)
        Fail("module info (p_paddr 0x%" PRIx64 ") is not inside the first segment", first->paddr);
    const u32 info_ea = static_cast<u32>(seg0.addr + rel);
    const auto info = memory_.ReadStruct<ppu_prx_module_info_header_t>(info_ea);

    // Table pointers are normally relocated to absolute EAs; accept segment-0-relative ones.
    const u32 lo = m.blocks.front(), hi = m.segments.back().addr + m.segments.back().memsz;
    auto fix = [&](u32 v) { return (v >= lo && v < hi) || v == 0 ? v : (v < seg0.memsz ? seg0.addr + v : v); };

    m.name.assign(info.name, strnlen(info.name, sizeof(info.name)));
    m.attributes = info.attributes;
    m.version[0] = info.version[0];
    m.version[1] = info.version[1];
    m.toc = fix(info.toc);
    const u32 ex_start = fix(info.exports_start), ex_end = fix(info.exports_end);
    const u32 im_start = fix(info.imports_start), im_end = fix(info.imports_end);

    // ---- exports ----
    if (ex_end < ex_start || !memory_.Check(ex_start, ex_end - ex_start, vm::kPageReadable))
        Fail("export table [0x%08x, 0x%08x) is invalid", ex_start, ex_end);
    for (u32 at = ex_start; at < ex_end;) {
        if (ex_end - at < sizeof(ppu_prx_export_info_t)) Fail("truncated export entry at 0x%08x", at);
        const auto e = memory_.ReadStruct<ppu_prx_export_info_t>(at);
        const u32 entry_size = e.size ? e.size : static_cast<u32>(sizeof(ppu_prx_export_info_t));
        const u32 count = u32{e.num_func} + e.num_var;
        if (count && (!memory_.Check(e.nids, count * 4, vm::kPageReadable) || !memory_.Check(e.addrs, count * 4, vm::kPageReadable)))
            Fail("export tables at 0x%08x point outside the module", at);
        PrxExportLibrary lib;
        lib.version = e.version;
        lib.attributes = e.attributes;
        const bool special = (e.attributes & 0x8000) != 0 || e.name == 0;
        if (!special) {
            const auto name = memory_.ReadCString(e.name, 128);
            if (!name) Fail("export entry at 0x%08x has an unreadable library name", at);
            lib.name = *name;
        }
        for (u32 i = 0; i < count; ++i) {
            const u32 nid = memory_.Read<u32>(e.nids + i * 4), addr = memory_.Read<u32>(e.addrs + i * 4);
            if (i < e.num_func) lib.functions.emplace_back(nid, addr);
            else lib.variables.emplace_back(nid, addr);
        }
        if (special) {
            for (const auto& [nid, addr] : lib.functions) {
                if (nid == kNidModuleStart) m.module_start = addr;
                else if (nid == kNidModuleStop) m.module_stop = addr;
                else if (nid == kNidModuleExit) m.module_exit = addr;
                const char* known = SpecialName(nid);
                LOG_INFO("prx", "  %s: special %s (0x%08X) at 0x%08x", m.name.c_str(), known ? known : "<unknown>", nid, addr);
            }
        } else {
            LOG_INFO("prx", "  %s exports %s: %zu functions, %zu variables", m.name.c_str(), lib.name.c_str(),
                     lib.functions.size(), lib.variables.size());
            if (!lib.variables.empty())
                LOG_WARN("prx", "  %s: %zu exported variable(s) of %s are not linked to importers yet", m.name.c_str(),
                         lib.variables.size(), lib.name.c_str());
            m.exports.push_back(std::move(lib));
        }
        at += entry_size;
    }
    for (const PrxExportLibrary& lib : m.exports) linker_.AddExports(id, lib.name, lib.functions);

    // ---- imports (LLE exports of other modules first, HLE otherwise) ----
    if (im_end > im_start) m.imports = linker_.BindImportTable(im_start, im_end, id);
}

PrxModule PrxLoader::Load(const PpuExecutable& exe, const std::string& path, u32 id) {
    PrxModule m;
    m.path = path;
    try {
        MapSegments(exe, m);
        Relocate(exe, m);
        ReadModuleInfo(exe, m, id);
        Protect(m);
    } catch (...) {
        linker_.RemoveModule(id);
        for (u32 block : m.blocks) memory_.Free(vm::Area::Main, block);
        throw;
    }
    const std::size_t relinked = linker_.RelinkAll();
    std::size_t imported = 0, lle = 0;
    for (const ImportedModule& im : m.imports)
        for (const ImportedFunction& f : im.functions) {
            ++imported;
            lle += f.lle_addr != 0;
        }
    LOG_INFO("prx",
             "loaded %s \"%s\" v%u.%u: %zu segment(s) at 0x%08x, %u relocations, toc 0x%08x, start 0x%08x, %zu export "
             "librar%s, %zu imports (%zu LLE), %zu existing import slot(s) relinked",
             path.c_str(), m.name.c_str(), m.version[0], m.version[1], m.segments.size(), m.segments.front().addr,
             m.relocations, m.toc, m.module_start, m.exports.size(), m.exports.size() == 1 ? "y" : "ies", imported, lle,
             relinked);
    return m;
}

void PrxLoader::Unload(PrxModule& m, u32 id) {
    linker_.RemoveModule(id);
    for (u32 block : m.blocks) memory_.Free(vm::Area::Main, block);
    m.blocks.clear();
}

}  // namespace cell::loader
