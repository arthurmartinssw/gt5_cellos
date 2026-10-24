// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/prx_tests.cpp
//
// Phase 7: PRX loader, dynamic linking (LLE over HLE), sys_prx and SPU group events.
//
// The PRX used here is synthetic but follows the real format byte for byte: ELF64 BE with
// e_type 0xFFA4, a text and a data PT_LOAD, a 0x700000A4 relocation table, module info found
// through p_paddr, a special export library (module_start, NID 0xBC9A0086), an exported
// library with three functions and an import of an HLE function. Its code is real PPU code
// run by the interpreter.
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include "hle/fnid.h"
#include "hle/sys_spu_hle.h"
#include "loader/ppu_linker.h"
#include "loader/prx_loader.h"
#include "lv2/sys_event.h"
#include "lv2/sys_prx.h"
#include "lv2/sys_spu.h"
#include "ppu_asm.h"
#include "spu/spu_interpreter.h"
#include "spu_asm.h"
#include "test_machine.h"

using namespace cell;

namespace {

// ---------------------------------------------------------------------------------------
// Synthetic PRX
// ---------------------------------------------------------------------------------------

constexpr u32 kNidAdd = 0x11111111, kNidAddr = 0x22222222, kNidBranch = 0x33333333;
constexpr u32 kTextOff = 0x100, kTextSize = 0x1000, kDataOff = 0x1100, kDataVaddr = 0x10000, kDataSize = 0x200;
constexpr u32 kRelOff = 0x1300;

struct Bytes {
    std::vector<u8> b;
    void Put(u32 at, u64 v, int size) {
        if (b.size() < at + size) b.resize(at + size);
        for (int i = 0; i < size; ++i) b[at + i] = static_cast<u8>(v >> (8 * (size - 1 - i)));
    }
    void Str(u32 at, const char* s) {
        for (u32 i = 0; s[i]; ++i) Put(at + i, static_cast<u8>(s[i]), 1);
        Put(at + static_cast<u32>(std::strlen(s)), 0, 1);
    }
};

std::vector<u8> BuildPrx() {
    using namespace as;
    Bytes text, data, rel;
    text.b.resize(kTextSize);
    data.b.resize(kDataSize);
    u32 relocs = 0;
    auto reloc = [&](u64 offset, u8 value_seg, u8 site_seg, u32 type, u64 addend) {
        const u32 at = relocs++ * 24;
        rel.Put(at + 0, offset, 8);
        rel.Put(at + 8, 0, 2);
        rel.Put(at + 10, value_seg, 1);
        rel.Put(at + 11, site_seg, 1);
        rel.Put(at + 12, type, 4);
        rel.Put(at + 16, addend, 8);
    };
    auto code = [&](u32 at, const std::vector<u32>& words) {
        for (std::size_t i = 0; i < words.size(); ++i) text.Put(at + static_cast<u32>(i * 4), words[i], 4);
    };
    // module_start: *marker = 0x5A; return SYS_PRX_RESIDENT
    code(0x000, {lwz(9, 0, 2), li(0, 0x5A), stw(0, 0, 9), li(3, 0), blr()});
    // prx_add(a, b) = test_add(a, b) + 1000, calling the imported HLE function through its slot
    code(0x040, {mflr(0), std_(0, 16, 1), stdu(1, -112, 1), std_(2, 40, 1), lwz(12, 4, 2), lwz(12, 0, 12), lwz(0, 0, 12),
                 mtctr(0), bctrl(), ld(2, 40, 1), addi(3, 3, 1000), addi(1, 1, 112), ld(0, 16, 1), mtlr(0), blr()});
    // prx_addr() = &marker via lis/addi patched by ADDR16_HA / ADDR16_LO
    code(0x100, {lis(3, 0), addi(3, 3, 0), blr()});
    reloc(0x102, 1, 0, loader::R_PPC64_ADDR16_HA, 0x30);
    reloc(0x106, 1, 0, loader::R_PPC64_ADDR16_LO, 0x30);
    // prx_branch(): b +? (REL24) -> li r3,77; blr at 0x180
    code(0x140, {b(0)});
    reloc(0x140, 0, 0, loader::R_PPC64_REL24, 0x180);
    code(0x180, {li(3, 77), blr()});

    // module info at text+0x200
    const u32 mi = 0x200;
    text.Put(mi + 0, 0x0000, 2);
    text.Put(mi + 2, 1, 1);
    text.Put(mi + 3, 2, 1);
    text.Str(mi + 4, "test_prx");
    reloc(mi + 32, 1, 0, loader::R_PPC64_ADDR32, 0x100);  // toc
    reloc(mi + 36, 0, 0, loader::R_PPC64_ADDR32, 0x240);  // exports_start
    reloc(mi + 40, 0, 0, loader::R_PPC64_ADDR32, 0x240 + 2 * 28);
    reloc(mi + 44, 0, 0, loader::R_PPC64_ADDR32, 0x280);  // imports_start
    reloc(mi + 48, 0, 0, loader::R_PPC64_ADDR32, 0x280 + 0x2C);
    // export 0: special library (module_start)
    text.Put(0x240 + 0, 0x1C, 1);
    text.Put(0x240 + 4, 0x8000, 2);
    text.Put(0x240 + 6, 1, 2);
    reloc(0x240 + 20, 0, 0, loader::R_PPC64_ADDR32, 0x300);  // nids
    reloc(0x240 + 24, 0, 0, loader::R_PPC64_ADDR32, 0x304);  // addrs
    text.Put(0x300, loader::kNidModuleStart, 4);
    reloc(0x304, 1, 0, loader::R_PPC64_ADDR32, 0x00);         // -> OPD module_start
    // export 1: "testLib" with three functions
    const u32 e1 = 0x240 + 28;
    text.Put(e1 + 0, 0x1C, 1);
    text.Put(e1 + 4, 0x0001, 2);
    text.Put(e1 + 6, 3, 2);
    reloc(e1 + 16, 0, 0, loader::R_PPC64_ADDR32, 0x340);  // name
    reloc(e1 + 20, 0, 0, loader::R_PPC64_ADDR32, 0x310);  // nids
    reloc(e1 + 24, 0, 0, loader::R_PPC64_ADDR32, 0x320);  // addrs
    text.Put(0x310, kNidAdd, 4);
    text.Put(0x314, kNidAddr, 4);
    text.Put(0x318, kNidBranch, 4);
    for (u32 i = 0; i < 3; ++i) reloc(0x320 + 4 * i, 1, 0, loader::R_PPC64_ADDR32, 0x08 + 8 * i);
    text.Str(0x340, "testLib");
    // import: "test" :: 0x1234 (test_add, HLE in Machine)
    const u32 im = 0x280;
    text.Put(im + 0, 0x2C, 1);
    text.Put(im + 2, 1, 2);
    text.Put(im + 4, 0x0001, 2);
    text.Put(im + 6, 1, 2);
    reloc(im + 16, 0, 0, loader::R_PPC64_ADDR32, 0x350);  // name
    reloc(im + 20, 0, 0, loader::R_PPC64_ADDR32, 0x308);  // nids
    reloc(im + 24, 1, 0, loader::R_PPC64_ADDR32, 0x20);   // slots (in data)
    text.Put(0x308, 0x1234, 4);
    text.Str(0x350, "test");

    // data: OPDs, slot, marker, relocation probes, TOC
    for (u32 i = 0; i < 4; ++i) {
        static constexpr u32 kCode[] = {0x000, 0x040, 0x100, 0x140};
        reloc(8 * i + 0, 0, 1, loader::R_PPC64_ADDR32, kCode[i]);
        reloc(8 * i + 4, 1, 1, loader::R_PPC64_ADDR32, 0x100);
    }
    reloc(0x40, 1, 1, loader::R_PPC64_ADDR64, 0x30);
    reloc(0x48, 1, 1, loader::R_PPC64_REL64, 0x30);
    reloc(0x50, 1, 1, loader::R_PPC64_ADDR16_HI, 0x30);
    data.Put(0x54, 0x1003, 2);  // low 2 bits must survive ADDR16_LO_DS
    reloc(0x54, 1, 1, loader::R_PPC64_ADDR16_LO_DS, 0x30);
    reloc(0x100, 1, 1, loader::R_PPC64_ADDR32, 0x30);  // TOC[0] = &marker
    reloc(0x104, 1, 1, loader::R_PPC64_ADDR32, 0x20);  // TOC[1] = &import slot

    // ELF
    Bytes f;
    f.b.resize(kRelOff);
    const u8 ident[] = {0x7F, 'E', 'L', 'F', 2, 2, 1, 0x66};
    for (u32 i = 0; i < sizeof(ident); ++i) f.Put(i, ident[i], 1);
    f.Put(0x10, 0xFFA4, 2);
    f.Put(0x12, 21, 2);
    f.Put(0x14, 1, 4);
    f.Put(0x20, 0x40, 8);  // phoff
    f.Put(0x34, 64, 2);
    f.Put(0x36, 56, 2);
    f.Put(0x38, 3, 2);
    auto phdr = [&](u32 i, u32 type, u32 flags, u64 off, u64 vaddr, u64 paddr, u64 filesz, u64 memsz) {
        const u32 at = 0x40 + i * 56;
        f.Put(at + 0, type, 4);
        f.Put(at + 4, flags, 4);
        f.Put(at + 8, off, 8);
        f.Put(at + 0x10, vaddr, 8);
        f.Put(at + 0x18, paddr, 8);
        f.Put(at + 0x20, filesz, 8);
        f.Put(at + 0x28, memsz, 8);
        f.Put(at + 0x30, 0x10000, 8);
    };
    phdr(0, loader::PT_LOAD, loader::PF_R | loader::PF_X, kTextOff, 0, kTextOff + mi, kTextSize, kTextSize);
    phdr(1, loader::PT_LOAD, loader::PF_R | loader::PF_W, kDataOff, kDataVaddr, 0, kDataSize, kDataSize);
    phdr(2, loader::PT_SCE_PPURELA, 0, kRelOff, 0, 0, rel.b.size(), 0);
    std::memcpy(f.b.data() + kTextOff, text.b.data(), kTextSize);
    std::memcpy(f.b.data() + kDataOff, data.b.data(), kDataSize);
    f.b.insert(f.b.end(), rel.b.begin(), rel.b.end());
    return f.b;
}

std::filesystem::path TempDir() {
    const auto dir = std::filesystem::temp_directory_path() / "cellos_prx_tests";
    std::filesystem::create_directories(dir / "sys" / "external");
    return dir;
}

std::filesystem::path WriteFile(const std::filesystem::path& p, const std::vector<u8>& bytes) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return p;
}

// An "EBOOT" importing testLib's three functions: libstub entry + nid array + slots in kData.
constexpr u32 kStub = kData + 0x800, kStubNids = kData + 0x840, kStubSlots = kData + 0x860, kStubName = kData + 0x880;
void ImportTestLib(Machine& m) {
    m.memory.Fill(kStub, 0, 0x2C);
    m.memory.Write<u8>(kStub, 0x2C);
    m.memory.Write<u16>(kStub + 2, 1);
    m.memory.Write<u16>(kStub + 4, 1);
    m.memory.Write<u16>(kStub + 6, 3);
    m.memory.Write<u32>(kStub + 16, kStubName);
    m.memory.Write<u32>(kStub + 20, kStubNids);
    m.memory.Write<u32>(kStub + 24, kStubSlots);
    m.memory.Write<u32>(kStubNids + 0, kNidAdd);
    m.memory.Write<u32>(kStubNids + 4, kNidAddr);
    m.memory.Write<u32>(kStubNids + 8, kNidBranch);
    m.memory.CopyIn(kStubName, "testLib", 8);
    (void)m.kernel->prx().linker().BindImportTable(kStub, kStub + 0x2C, loader::PpuLinker::kMainExecutable);
}

// Calls slot `index` of the imported testLib like compiled PS3 code: OPD code + toc.
u64 CallImport(Machine& m, u32 index, u64 a, u64 b) {
    using namespace as;
    Program p;
    p << mflr(31) << lis(30, static_cast<s32>(kStubSlots >> 16)) << ori(30, 30, kStubSlots & 0xFFFF);
    p << lwz(12, static_cast<s32>(index * 4), 30) << lwz(0, 0, 12) << lwz(2, 4, 12) << mtctr(0);
    p << li(3, static_cast<s32>(a)) << li(4, static_cast<s32>(b)) << bctrl() << mtlr(31) << blr();
    const auto stop = m.Run(p.words(), 10000);
    CHECK(stop.reason == ppu::StopReason::Exited);
    return m.ctx.gpr[3];
}

// ---------------------------------------------------------------------------------------

void TestRelocationMath() {
    std::printf("-- PRX relocation types\n");
    Machine m;
    const u32 s = kData + 0x100;
    loader::ApplyPrxRelocation(m.memory, loader::R_PPC64_ADDR32, s, 0x12345678);
    CHECK_EQ(m.memory.Read<u32>(s), 0x12345678);
    loader::ApplyPrxRelocation(m.memory, loader::R_PPC64_ADDR16_LO, s, 0x12348765);
    loader::ApplyPrxRelocation(m.memory, loader::R_PPC64_ADDR16_HA, s + 2, 0x12348765);
    CHECK_EQ(m.memory.Read<u16>(s), 0x8765);
    CHECK_EQ(m.memory.Read<u16>(s + 2), 0x1235);  // high adjusted: low half is negative as a signed immediate
    loader::ApplyPrxRelocation(m.memory, loader::R_PPC64_ADDR16_HI, s + 4, 0x12348765);
    CHECK_EQ(m.memory.Read<u16>(s + 4), 0x1234);
    m.memory.Write<u32>(s + 8, 0x48000001);  // bl 0
    loader::ApplyPrxRelocation(m.memory, loader::R_PPC64_REL24, s + 8, s + 8 - 0x100);
    CHECK_EQ(m.memory.Read<u32>(s + 8), 0x4BFFFF01);  // bl -0x100, link bit kept
    m.memory.Write<u32>(s + 12, 0x41820000);  // beq 0
    loader::ApplyPrxRelocation(m.memory, loader::R_PPC64_REL14, s + 12, s + 12 + 0x40);
    CHECK_EQ(m.memory.Read<u32>(s + 12), 0x41820040);
    loader::ApplyPrxRelocation(m.memory, loader::R_PPC64_ADDR64, s + 16, 0x1122334455667788ull);
    CHECK_EQ(m.memory.Read<u64>(s + 16), 0x1122334455667788ull);
    bool threw = false;
    try {
        loader::ApplyPrxRelocation(m.memory, loader::R_PPC64_REL24, s, s + 0x4000000);
    } catch (const loader::LoadError&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        loader::ApplyPrxRelocation(m.memory, 99, s, 0);
    } catch (const loader::LoadError& e) {
        threw = std::string(e.what()).find("unsupported relocation type 99") != std::string::npos;
    }
    CHECK(threw);
}

void TestLoadLinkRun() {
    std::printf("-- PRX load: segments, relocations, module info, exports, imports, LLE relink, module_start\n");
    Machine m;
    ImportTestLib(m);  // the EBOOT's testLib imports start as HLE stubs
    const u32 stub_opd = m.memory.Read<u32>(kStubSlots);
    const auto path = WriteFile(TempDir() / "test.sprx", BuildPrx());
    u32 id = 0;
    CHECK_EQ(m.kernel->prx().LoadFile(path, "/host/test.sprx", &id), CELL_OK);
    const auto prx = m.kernel->prx().Find(id);
    CHECK(prx != nullptr);
    if (!prx) return;
    const loader::PrxModule& mod = prx->module;
    CHECK(mod.name == "test_prx");
    CHECK_EQ(mod.version[0], 1);
    CHECK_EQ(mod.segments.size(), 2);
    const u32 text = mod.segments[0].addr, data = mod.segments[1].addr;
    CHECK_EQ(data - text, kDataVaddr);  // relative layout kept
    CHECK_EQ(mod.toc, data + 0x100);
    CHECK_EQ(mod.module_start, data + 0x00);
    CHECK_EQ(mod.exports.size(), 1);
    CHECK(mod.exports[0].name == "testLib" && mod.exports[0].functions.size() == 3);
    // relocated data
    CHECK_EQ(m.memory.Read<u32>(data + 0x08), text + 0x40);   // OPD code
    CHECK_EQ(m.memory.Read<u32>(data + 0x0C), data + 0x100);  // OPD toc
    CHECK_EQ(m.memory.Read<u32>(data + 0x100), data + 0x30);  // TOC[0]
    CHECK_EQ(m.memory.Read<u64>(data + 0x40), data + 0x30);   // ADDR64
    CHECK_EQ(m.memory.Read<u64>(data + 0x48), static_cast<u64>(-0x18));  // REL64
    CHECK_EQ(m.memory.Read<u16>(data + 0x50), (data + 0x30) >> 16);      // ADDR16_HI
    CHECK_EQ(m.memory.Read<u16>(data + 0x54), ((data + 0x30) & 0xFFFC) | 3);  // ADDR16_LO_DS
    // the PRX's own import -> HLE test_add
    const u32 import_slot = m.memory.Read<u32>(data + 0x20);
    u32 index = 0;
    CHECK(hle::DecodeHleOpcode(m.memory.Read<u32>(m.memory.Read<u32>(import_slot)), &index));
    CHECK(m.hle->At(index).name == "test_add");
    // text is read-only code, data writable
    CHECK(!m.memory.Check(text, 4, vm::kPageWritable));
    CHECK(m.memory.Flags(text) & vm::kPageCode);
    CHECK(m.memory.Check(data, 4, vm::kPageWritable));
    // the EBOOT's slots moved from HLE stubs to the module's OPDs
    CHECK(m.memory.Read<u32>(kStubSlots) != stub_opd);
    CHECK_EQ(m.memory.Read<u32>(kStubSlots + 0), data + 0x08);
    CHECK_EQ(m.memory.Read<u32>(kStubSlots + 8), data + 0x18);
    CHECK(m.kernel->prx().linker().IsLle("testLib", kNidAdd));

    // module_start runs on a PPU thread and returns SYS_PRX_RESIDENT
    s32 result = -1;
    CHECK_EQ(m.kernel->prx().StartModule(id, 0, 0, &result), CELL_OK);
    CHECK_EQ(result, 0);
    CHECK_EQ(m.memory.Read<u32>(data + 0x30), 0x5A);

    // calls from the "EBOOT" into the module, and from the module into HLE
    CHECK_EQ(CallImport(m, 0, 5, 7), 1012);
    CHECK_EQ(CallImport(m, 1, 0, 0), data + 0x30);
    CHECK_EQ(CallImport(m, 2, 0, 0), 77);
    CHECK(!m.kernel->is_terminating());
    CHECK(!m.kernel->prx().Describe().empty());
}

void TestSysPrx() {
    std::printf("-- sys_prx: VFS path, raw syscalls, user-side HLE, two-phase start/stop, unload + relink to HLE\n");
    Machine m;
    ImportTestLib(m);
    const u32 stub_opd = m.memory.Read<u32>(kStubSlots);
    const auto dir = TempDir();
    WriteFile(dir / "sys" / "external" / "libtest.sprx", BuildPrx());
    WriteFile(dir / "sys" / "external" / "broken.sprx", std::vector<u8>(64, 0x42));
    m.kernel->vfs().Mount("/dev_flash", dir);

    auto sys = [&](u32 n, std::initializer_list<u64> args) {
        ppu::PpuThreadContext c{};
        c.gpr[11] = n;
        int r = 3;
        for (u64 a : args) c.gpr[r++] = a;
        m.syscalls.Dispatch(c);
        return static_cast<s32>(c.gpr[3]);
    };
    auto hle_call = [&](const char* name, std::initializer_list<u64> args) {
        const u32 index = m.hle->Bind("sysPrxForUser", hle::ComputeFnid(name));
        const auto& f = m.hle->At(index);
        ppu::PpuThreadContext c{};
        int r = 3;
        for (u64 a : args) c.gpr[r++] = a;
        f.fn(c, m.memory, f);
        return static_cast<s32>(c.gpr[3]);
    };
    const u32 path = kData + 0x100, name = kData + 0x180, modres = kData + 0x1C0, opt = kData + 0x200;
    m.memory.CopyIn(name, "test_prx", 9);

    const char missing[] = "/dev_flash/sys/external/nope.sprx";
    m.memory.CopyIn(path, missing, sizeof(missing));
    CHECK_EQ(sys(480, {path, 0, 0}), CELL_ENOENT);
    const char broken[] = "/dev_flash/sys/external/broken.sprx";
    m.memory.CopyIn(path, broken, sizeof(broken));
    CHECK_EQ(sys(480, {path, 0, 0}), CELL_ENOEXEC);
    const char escape[] = "/dev_flash/../secret.sprx";
    m.memory.CopyIn(path, escape, sizeof(escape));
    CHECK_EQ(sys(480, {path, 0, 0}), CELL_ENOENT);

    const char good[] = "/dev_flash/sys/external/libtest.sprx";
    m.memory.CopyIn(path, good, sizeof(good));
    const s32 id = hle_call("sys_prx_load_module", {path, 0, 0});
    CHECK(static_cast<u32>(id) >> 24 == lv2::kIdPrx >> 24);
    CHECK_EQ(sys(496, {name, 0, 0}), id);
    CHECK_EQ(hle_call("sys_prx_get_module_id_by_name", {name, 0, 0}), id);
    CHECK_EQ(hle_call("sys_prx_start_module", {static_cast<u32>(id), 0, 0, modres, 0, 0}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(modres), 0);
    CHECK_EQ(hle_call("sys_prx_start_module", {static_cast<u32>(id), 0, 0, modres, 0, 0}), lv2::CELL_PRX_ERROR_ALREADY_STARTED);
    CHECK_EQ(sys(483, {static_cast<u32>(id), 0, 0}), lv2::CELL_PRX_ERROR_NOT_REMOVABLE);
    CHECK_EQ(CallImport(m, 0, 1, 2), 1003);
    CHECK_EQ(hle_call("sys_prx_stop_module", {static_cast<u32>(id), 0, 0, modres, 0, 0}), CELL_OK);
    CHECK_EQ(hle_call("sys_prx_unload_module", {static_cast<u32>(id), 0, 0}), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kStubSlots), stub_opd);  // back on the HLE stub
    CHECK(!m.kernel->prx().linker().IsLle("testLib", kNidAdd));
    CHECK_EQ(sys(496, {name, 0, 0}), lv2::CELL_PRX_ERROR_UNKNOWN_MODULE);

    // Two-phase raw protocol (what liblv2's own wrapper does)
    const s32 id2 = sys(480, {path, 0, 0});
    CHECK(id2 > 0);
    m.memory.Fill(opt, 0, 40);
    m.memory.Write<u64>(opt + 0, 40);
    m.memory.Write<u64>(opt + 8, 1);
    CHECK_EQ(sys(481, {static_cast<u32>(id2), 0, opt}), CELL_OK);
    const u32 entry = static_cast<u32>(m.memory.Read<u64>(opt + 16));
    CHECK_EQ(entry, m.kernel->prx().Find(static_cast<u32>(id2))->module.module_start);
    const auto r = m.kernel->threads().CallGuest(entry, 0, 0, "module_start");
    CHECK(r && *r == 0);
    m.memory.Write<u64>(opt + 8, 2);
    m.memory.Write<u64>(opt + 24, *r);
    CHECK_EQ(sys(481, {static_cast<u32>(id2), 0, opt}), CELL_OK);
    CHECK(m.kernel->prx().Find(static_cast<u32>(id2))->state == lv2::Lv2Prx::State::Started);
    m.memory.Write<u64>(opt + 8, 1);
    CHECK_EQ(sys(482, {static_cast<u32>(id2), 0, opt}), CELL_OK);
    CHECK_EQ(m.memory.Read<u64>(opt + 16), ~u64{0});  // no module_stop in this module
    m.memory.Write<u64>(opt + 8, 2);
    CHECK_EQ(sys(482, {static_cast<u32>(id2), 0, opt}), CELL_OK);
    CHECK_EQ(sys(483, {static_cast<u32>(id2), 0, 0}), CELL_OK);
    CHECK(!m.kernel->is_terminating());
}

// ---------------------------------------------------------------------------------------
// SPU group events
// ---------------------------------------------------------------------------------------

struct SpuSetup {
    Machine m;
    hle::SysSpuHle spu_hle{*m.kernel};
    u32 group = 0, thread = 0;

    s32 Sys(u32 n, std::initializer_list<u64> args) {
        ppu::PpuThreadContext c{};
        c.gpr[11] = n;
        int r = 3;
        for (u64 a : args) c.gpr[r++] = a;
        m.syscalls.Dispatch(c);
        return static_cast<s32>(c.gpr[3]);
    }
    ppu::PpuThreadContext SysCtx(u32 n, std::initializer_list<u64> args) {
        ppu::PpuThreadContext c{};
        c.gpr[11] = n;
        int r = 3;
        for (u64 a : args) c.gpr[r++] = a;
        m.syscalls.Dispatch(c);
        return c;
    }
    u32 Queue(u32 out) {
        m.memory.Write<u32>(kData + 0x300, lv2::SYS_SYNC_FIFO);
        m.memory.Write<u32>(kData + 0x304, lv2::SYS_PPU_QUEUE);
        m.memory.Write<u64>(kData + 0x308, 0);
        CHECK_EQ(Sys(128, {out, kData + 0x300, 0, 16}), CELL_OK);
        return m.memory.Read<u32>(out);
    }
    // One SPU thread running `code` at LS 0x100.
    explicit SpuSetup(const std::vector<u32>& code) {
        spu_hle.Register(*m.hle);
        m.kernel->spu().SetExecutorFactory(spu::MakeSpuInterpreter);
        const u32 elf = kData + 0x1000, payload = 0x80;
        m.memory.Fill(elf, 0, 0x100);
        const u8 ident[] = {0x7F, 'E', 'L', 'F', 1, 2, 1};
        m.memory.CopyIn(elf, ident, sizeof(ident));
        m.memory.Write<u16>(elf + 16, 2);
        m.memory.Write<u16>(elf + 18, 23);
        m.memory.Write<u32>(elf + 24, 0x100);
        m.memory.Write<u32>(elf + 28, 52);
        m.memory.Write<u16>(elf + 42, 32);
        m.memory.Write<u16>(elf + 44, 1);
        m.memory.Write<u32>(elf + 52, 1);
        m.memory.Write<u32>(elf + 56, payload);
        m.memory.Write<u32>(elf + 60, 0x100);
        m.memory.Write<u32>(elf + 68, static_cast<u32>(code.size() * 4));
        m.memory.Write<u32>(elf + 72, static_cast<u32>(code.size() * 4));
        for (std::size_t k = 0; k < code.size(); ++k) m.memory.Write<u32>(elf + payload + 4 * static_cast<u32>(k), code[k]);
        const u32 img = kData + 0x400;
        const u32 index = m.hle->Bind("sysPrxForUser", hle::ComputeFnid("sys_spu_image_import"));
        const auto& f = m.hle->At(index);
        ppu::PpuThreadContext c{};
        c.gpr[3] = img;
        c.gpr[4] = elf;
        c.gpr[5] = spu::SYS_SPU_IMAGE_PROTECT;
        f.fn(c, m.memory, f);
        CHECK_EQ(static_cast<s32>(c.gpr[3]), CELL_OK);
        m.memory.Fill(kData + 0x500, 0, 0x60);
        CHECK_EQ(Sys(170, {kData + 0x580, 1, 100, kData + 0x500}), CELL_OK);
        group = m.memory.Read<u32>(kData + 0x580);
        CHECK_EQ(Sys(172, {kData + 0x584, group, 0, img, kData + 0x520, kData + 0x530}), CELL_OK);
        thread = m.memory.Read<u32>(kData + 0x584);
    }
};

void TestGroupEvents() {
    std::printf("-- SPU group events: RUN on start, connect_event_all_threads + send_event, EXCEPTION on a fault\n");
    {
        // SPU: sys_spu_thread_send_event(port 5, 0x42, 0x99), exit with the reply
        std::vector<u32> code{sa::Il(3, 0x99), sa::Wrch(28, 3)};
        for (u32 w : sa::Load32(4, 0x05000042)) code.push_back(w);
        code.insert(code.end(), {sa::Wrch(30, 4), sa::Rdch(5, 29), sa::Wrch(28, 5), sa::Stop(0x102)});
        SpuSetup s(code);
        const u32 run_q = s.Queue(kData + 0x600), user_q = s.Queue(kData + 0x604);
        CHECK_EQ(s.Sys(185, {s.group, run_q, lv2::SYS_SPU_THREAD_GROUP_EVENT_RUN}), CELL_OK);
        CHECK_EQ(s.Sys(251, {s.group, user_q, 0, kData + 0x608}), CELL_EINVAL);           // empty request mask
        CHECK_EQ(s.Sys(251, {s.group, user_q, u64{1} << 5, kData + 0x608}), CELL_OK);
        CHECK_EQ(s.m.memory.Read<u8>(kData + 0x608), 5);
        CHECK_EQ(s.Sys(251, {s.group, user_q, u64{1} << 5, kData + 0x608}), CELL_EISCONN);  // port taken
        CHECK_EQ(s.Sys(250, {s.group, 1}), CELL_OK);
        CHECK_EQ(s.Sys(173, {s.group}), CELL_OK);
        const auto run = s.SysCtx(130, {run_q, 0, 0});
        CHECK_EQ(static_cast<s32>(run.gpr[3]), CELL_OK);
        CHECK_EQ(run.gpr[4], spu::SYS_SPU_THREAD_GROUP_EVENT_RUN_KEY);
        CHECK_EQ(run.gpr[5], s.group);
        const auto user = s.SysCtx(130, {user_q, 0, 0});
        CHECK_EQ(user.gpr[4], spu::SYS_SPU_THREAD_EVENT_USER_KEY);
        CHECK_EQ(user.gpr[5], s.thread);
        CHECK_EQ(user.gpr[6], (u64{5} << 32) | 0x42);
        CHECK_EQ(user.gpr[7], 0x99);
        CHECK_EQ(s.Sys(178, {s.group, kData + 0x610, kData + 0x614}), CELL_OK);
        CHECK_EQ(s.Sys(165, {s.thread, kData + 0x618}), CELL_OK);
        CHECK_EQ(s.m.memory.Read<u32>(kData + 0x618), 0);  // CELL_OK reply from the event send
        CHECK_EQ(s.Sys(252, {s.group, 5}), CELL_OK);
        CHECK_EQ(s.Sys(252, {s.group, 5}), CELL_ENOTCONN);
    }
    {
        SpuSetup s({sa::Il(3, 1), 0x00800000});  // invalid instruction
        const u32 q = s.Queue(kData + 0x600);
        CHECK_EQ(s.Sys(185, {s.group, q, lv2::SYS_SPU_THREAD_GROUP_EVENT_EXCEPTION}), CELL_OK);
        CHECK_EQ(s.Sys(173, {s.group}), CELL_OK);
        // the queue gets the event even though the process is ending: read it directly
        const auto queue = s.m.kernel->objects().Get<lv2::Lv2EventQueue>(q);
        bool got = false;
        for (int i = 0; i < 500 && !got; ++i) {
            {
                std::lock_guard lock(queue->m);
                if (!queue->events.empty()) {
                    const auto e = queue->events.front();
                    got = e.source == spu::SYS_SPU_THREAD_GROUP_EVENT_EXCEPTION_KEY && e.data1 == s.group && e.data2 == s.thread;
                }
            }
            if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(got);
        CHECK(s.m.kernel->is_terminating());
    }
}

// Phase 7.7: sys_prx_get_module_list (sysPrxForUser, NID 0xA5D06BF0) and syscall 494. The
// output count must always be written; liblv2.sprx is never listed (RPCS3 lv2/sys_prx.cpp).
void TestModuleList() {
    std::printf("-- sys_prx_get_module_list / _sys_prx_get_module_list (494): ids, max, count, liblv2 hidden\n");
    CHECK_EQ(hle::ComputeFnid("sys_prx_get_module_list"), 0xA5D06BF0u);
    Machine m;
    const auto path = WriteFile(TempDir() / "list.sprx", BuildPrx());
    u32 a = 0, b = 0, lv2 = 0;
    CHECK_EQ(m.kernel->prx().LoadFile(path, "/host/list.sprx", &a), CELL_OK);
    CHECK_EQ(m.kernel->prx().LoadFile(path, "/dev_flash/sys/external/liblv2.sprx", &lv2), CELL_OK);
    CHECK_EQ(m.kernel->prx().LoadFile(path, "/host/list2.sprx", &b), CELL_OK);
    auto call = [&](u64 flags, u32 info) {
        ppu::PpuThreadContext c{};
        c.gpr[3] = flags;
        c.gpr[4] = info;
        const hle::HleFunction& f = m.hle->At(m.hle->Bind("sysPrxForUser", hle::ComputeFnid("sys_prx_get_module_list")));
        CHECK(f.implemented);
        f.fn(c, m.memory, f);
        return static_cast<s32>(c.gpr[3]);
    };
    constexpr u32 kInfo = kData + 0x800, kIds = kData + 0x900;
    // info { be64 size; be32 max; be32 count; be32 idlist* }
    auto prepare = [&](u32 max, u32 idlist) {
        m.memory.Write<u64>(kInfo, 0x18);
        m.memory.Write<u32>(kInfo + 8, max);
        m.memory.Write<u32>(kInfo + 12, 0xDEADBEEF);  // must be overwritten
        m.memory.Write<u32>(kInfo + 16, idlist);
        for (u32 i = 0; i < 8; ++i) m.memory.Write<u32>(kIds + 4 * i, 0xCCCCCCCC);
    };
    CHECK_EQ(call(0, 0), CELL_EINVAL);
    prepare(8, 0);
    CHECK_EQ(call(0, kInfo), CELL_EINVAL);  // null idlist
    CHECK_EQ(m.memory.Read<u32>(kInfo + 12), 0xDEADBEEF);
    prepare(8, kIds);
    CHECK_EQ(call(0, kInfo), CELL_OK);  // flags ignored by the user library (it asks lv2 with 2)
    CHECK_EQ(m.memory.Read<u32>(kInfo + 8), 8);
    CHECK_EQ(m.memory.Read<u32>(kInfo + 12), 2);
    CHECK_EQ(m.memory.Read<u32>(kIds + 0), a);
    CHECK_EQ(m.memory.Read<u32>(kIds + 4), b);
    CHECK_EQ(m.memory.Read<u32>(kIds + 8), 0xCCCCCCCC);
    prepare(1, kIds);
    CHECK_EQ(call(0, kInfo), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kInfo + 12), 1);
    CHECK_EQ(m.memory.Read<u32>(kIds + 4), 0xCCCCCCCC);
    prepare(0, kIds);
    CHECK_EQ(call(0, kInfo), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kInfo + 12), 0);

    // Raw syscall: opt { be64 size 0x20; pad; max; count; idlist; unk }
    auto sys = [&](u64 flags, u32 opt) {
        ppu::PpuThreadContext c{};
        c.gpr[11] = 494;
        c.gpr[3] = flags;
        c.gpr[4] = opt;
        m.syscalls.Dispatch(c);
        return static_cast<s32>(c.gpr[3]);
    };
    constexpr u32 kOpt = kData + 0xA00;
    m.memory.Write<u64>(kOpt, 0x20);
    m.memory.Write<u32>(kOpt + 12, 4);
    m.memory.Write<u32>(kOpt + 16, 0xDEADBEEF);
    m.memory.Write<u32>(kOpt + 20, kIds);
    CHECK_EQ(sys(0, kOpt), CELL_OK);  // no 0x2 flag: nothing written
    CHECK_EQ(m.memory.Read<u32>(kOpt + 16), 0xDEADBEEF);
    CHECK_EQ(sys(2, kOpt), CELL_OK);
    CHECK_EQ(m.memory.Read<u32>(kOpt + 16), 2);
    m.memory.Write<u64>(kOpt, 0x18);
    m.memory.Write<u32>(kOpt + 16, 0xDEADBEEF);
    CHECK_EQ(sys(2, kOpt), CELL_OK);  // unknown structure: nothing written
    CHECK_EQ(m.memory.Read<u32>(kOpt + 16), 0xDEADBEEF);
    CHECK_EQ(sys(2, 0), CELL_EFAULT);
}

}  // namespace

int main() {
    log::Threshold() = log::Level::Error;
    TestRelocationMath();
    TestLoadLinkRun();
    TestSysPrx();
    TestGroupEvents();
    TestModuleList();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all PRX tests passed\n");
    return 0;
}
