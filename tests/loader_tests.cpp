// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/loader_tests.cpp
//
// Self-contained tests: synthesize a big-endian PPC64 EBOOT (plain ELF, debug SELF with a
// zlib segment, retail SELF) and run it through the real loader and memory code.
// No game data needed; mirrors bloodborne_pc/tests/test_prepare.py, which builds fixtures
// with struct.pack_into the same way.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <zlib.h>

#include "hle/hle_registry.h"
#include "loader/ppu_loader.h"
#include "loader/self.h"
#include "lv2/sys_memory.h"
#include "memory/guest_memory.h"

using namespace cell;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

namespace {

void Put16(std::vector<u8>& b, std::size_t at, u16 v) { b[at] = u8(v >> 8); b[at + 1] = u8(v); }
void Put32(std::vector<u8>& b, std::size_t at, u32 v) { for (int i = 0; i < 4; ++i) b[at + i] = u8(v >> (24 - 8 * i)); }
void Put64(std::vector<u8>& b, std::size_t at, u64 v) { for (int i = 0; i < 8; ++i) b[at + i] = u8(v >> (56 - 8 * i)); }

constexpr u32 kText = 0x10000, kData = 0x20000;
constexpr u32 kOpd = kData + 0x000, kProc = kData + 0x010, kPrx = kData + 0x040, kStubs = kData + 0x080;
constexpr u32 kNids1 = kData + 0x100, kAddrs1 = kData + 0x110, kNids2 = kData + 0x120, kAddrs2 = kData + 0x130;
constexpr u32 kName1 = kData + 0x140, kName2 = kData + 0x150, kTls = kData + 0x160;
constexpr u32 kNidGcmInit = 0x15BAE46B, kNidOther = 0x21AC3697, kNidFsOpen = 0x718BF5F8;  // arbitrary test values
constexpr u32 kTextOff = 0x1000, kDataOff = 0x2000, kDataFilesz = 0x200;

struct Phdr { u32 type, flags; u64 offset, vaddr, filesz, memsz, align; };

std::vector<Phdr> Phdrs() {
    return {
        {loader::PT_LOAD, loader::PF_R | loader::PF_X, kTextOff, kText, 0x100, 0x100, 0x10000},
        {loader::PT_LOAD, loader::PF_R | loader::PF_W, kDataOff, kData, kDataFilesz, 0x1000, 0x10000},
        {loader::PT_TLS, loader::PF_R, kDataOff + 0x160, kTls, 8, 0x20, 8},
        {loader::PT_PROC_PARAM, loader::PF_R, kDataOff + 0x010, kProc, 0x20, 0x20, 4},
        {loader::PT_PROC_PRX_PARAM, loader::PF_R, kDataOff + 0x040, kPrx, 0x20, 0x20, 4},
    };
}

void WriteElfHeader(std::vector<u8>& b, std::size_t at, u64 phoff, u16 phnum) {
    const u8 ident[16] = {0x7F, 'E', 'L', 'F', 2, 2, 1, 0x66};
    std::memcpy(&b[at], ident, 16);
    Put16(b, at + 0x10, loader::ET_EXEC);
    Put16(b, at + 0x12, loader::EM_PPC64);
    Put32(b, at + 0x14, 1);
    Put64(b, at + 0x18, kOpd);  // e_entry -> OPD
    Put64(b, at + 0x20, phoff);
    Put16(b, at + 0x34, 64);
    Put16(b, at + 0x36, 56);
    Put16(b, at + 0x38, phnum);
}

void WritePhdrs(std::vector<u8>& b, std::size_t at, const std::vector<Phdr>& ph) {
    for (std::size_t i = 0; i < ph.size(); ++i) {
        const std::size_t p = at + i * 56;
        Put32(b, p, ph[i].type);
        Put32(b, p + 4, ph[i].flags);
        Put64(b, p + 8, ph[i].offset);
        Put64(b, p + 0x10, ph[i].vaddr);
        Put64(b, p + 0x18, ph[i].vaddr);
        Put64(b, p + 0x20, ph[i].filesz);
        Put64(b, p + 0x28, ph[i].memsz);
        Put64(b, p + 0x30, ph[i].align);
    }
}

// The two segments' bytes (text: nop; blr — data: OPD, params, libstub tables, TLS image).
std::vector<u8> TextBytes() {
    std::vector<u8> t(0x100, 0);
    Put32(t, 0, 0x60000000);  // nop
    Put32(t, 4, 0x4E800020);  // blr
    return t;
}
std::vector<u8> DataBytes() {
    std::vector<u8> d(kDataFilesz, 0);
    auto at = [](u32 ea) { return static_cast<std::size_t>(ea - kData); };
    Put32(d, at(kOpd), kText);
    Put32(d, at(kOpd) + 4, kData + 0x8000);  // TOC
    // sys_process_param_t
    Put32(d, at(kProc) + 0x00, 0x20);
    Put32(d, at(kProc) + 0x04, loader::kProcParamMagic);
    Put32(d, at(kProc) + 0x08, 0x330000);
    Put32(d, at(kProc) + 0x0C, 0x00360001);
    Put32(d, at(kProc) + 0x10, 1001);
    Put32(d, at(kProc) + 0x14, 0x40000);   // 256 KiB primary stack
    Put32(d, at(kProc) + 0x18, 0x100000);  // malloc page size 1M
    // sys_process_prx_info_t
    Put32(d, at(kPrx) + 0x00, 0x20);
    Put32(d, at(kPrx) + 0x04, loader::kPrxParamMagic);
    Put32(d, at(kPrx) + 0x18, kStubs);
    Put32(d, at(kPrx) + 0x1C, kStubs + 2 * 0x2C);
    // libstub[0]: cellGcmSys, 2 functions
    auto stub = [&](u32 ea, u16 nfunc, u32 name, u32 nids, u32 addrs) {
        d[at(ea)] = 0x2C;
        Put16(d, at(ea) + 2, 1);
        Put16(d, at(ea) + 4, 0x0009);
        Put16(d, at(ea) + 6, nfunc);
        Put32(d, at(ea) + 0x10, name);
        Put32(d, at(ea) + 0x14, nids);
        Put32(d, at(ea) + 0x18, addrs);
    };
    stub(kStubs, 2, kName1, kNids1, kAddrs1);
    stub(kStubs + 0x2C, 1, kName2, kNids2, kAddrs2);
    Put32(d, at(kNids1), kNidGcmInit);
    Put32(d, at(kNids1) + 4, kNidOther);
    Put32(d, at(kNids2), kNidFsOpen);
    std::memcpy(&d[at(kName1)], "cellGcmSys", 11);
    std::memcpy(&d[at(kName2)], "sys_fs", 7);
    std::memcpy(&d[at(kTls)], "TLSDATA!", 8);
    return d;
}

std::vector<u8> BuildElf() {
    const auto ph = Phdrs();
    std::vector<u8> b(kDataOff + kDataFilesz, 0);
    WriteElfHeader(b, 0, 64, static_cast<u16>(ph.size()));
    WritePhdrs(b, 64, ph);
    const auto text = TextBytes(), data = DataBytes();
    std::memcpy(&b[kTextOff], text.data(), text.size());
    std::memcpy(&b[kDataOff], data.data(), data.size());
    return b;
}

// SELF wrapper: SCE header, extended header, app info, ELF header, phdrs, section info,
// then segment data (data segment zlib-compressed).
std::vector<u8> BuildSelf(u16 key_revision) {
    const auto ph = Phdrs();
    const std::size_t app_info = 0x70, elf = 0x90, phoff = elf + 0x40, sec_info = phoff + ph.size() * 56;
    std::size_t cursor = AlignUp<std::size_t>(sec_info + ph.size() * 0x20, 16);
    std::vector<u8> b(cursor, 0);
    Put32(b, 0x00, loader::kSceMagic);
    Put32(b, 0x04, 2);
    Put16(b, 0x08, key_revision);
    Put16(b, 0x0A, loader::kSceHeaderTypeSelf);
    Put64(b, 0x10, cursor);
    Put64(b, 0x20, 3);
    Put64(b, 0x28, app_info);
    Put64(b, 0x30, elf);
    Put64(b, 0x38, phoff);
    Put64(b, 0x48, sec_info);
    Put64(b, app_info, 0x1010000001000003ull);
    Put32(b, app_info + 0x0C, 4);
    Put64(b, app_info + 0x10, 0x0001000000000000ull);
    WriteElfHeader(b, elf, 0, static_cast<u16>(ph.size()));
    WritePhdrs(b, phoff, ph);

    const auto text = TextBytes(), data = DataBytes();
    std::vector<u8> compressed(compressBound(data.size()));
    uLongf clen = compressed.size();
    compress2(compressed.data(), &clen, data.data(), data.size(), 9);
    compressed.resize(clen);

    auto append = [&](const std::vector<u8>& bytes) {
        const std::size_t at = b.size();
        b.insert(b.end(), bytes.begin(), bytes.end());
        b.resize(AlignUp<std::size_t>(b.size(), 16), 0);
        return at;
    };
    const std::size_t text_at = append(text), data_at = append(compressed);
    for (std::size_t i = 0; i < ph.size(); ++i) {
        const std::size_t e = sec_info + i * 0x20;
        u64 off = 0, size = 0;
        u32 comp = 1;
        if (i == 0) { off = text_at; size = text.size(); }
        else if (i == 1) { off = data_at; size = compressed.size(); comp = 2; }
        else { off = data_at; size = 0; }  // TLS/params live inside the data segment
        Put64(b, e, off);
        Put64(b, e + 8, size);
        Put32(b, e + 0x10, comp);
        Put32(b, e + 0x1C, 2);  // not encrypted
    }
    return b;
}

bool g_hle_called = false;
void FakeGcmInit(ppu::PpuThreadContext& ctx, vm::GuestMemory&, const hle::HleFunction&) {
    g_hle_called = true;
    ctx.gpr[3] = 0x1234;
}

void CheckLoaded(vm::GuestMemory& mem, hle::HleRegistry& hle, const loader::PpuBootImage& img,
                 const ppu::PpuThreadContext& ctx) {
    CHECK(img.entry_opd == kOpd);
    CHECK(img.entry_pc == kText);
    CHECK(img.entry_toc == kData + 0x8000);
    CHECK(mem.Read<u32>(kText) == 0x60000000);
    CHECK(img.proc.present && img.proc.primary_stacksize == 0x40000 && img.proc.sdk_version == 0x00360001);

    // Page protection and JIT code tracking.
    CHECK((mem.Flags(kText) & vm::kPageCode) && (mem.Flags(kText) & vm::kPageReadable));
    CHECK(!(mem.Flags(kText) & vm::kPageWritable));
    CHECK(mem.Flags(kData + 0x800) & vm::kPageWritable);  // BSS
    CHECK(mem.Read<u32>(kData + 0x800) == 0);

    // Imports: 2 modules, 3 functions, every slot -> OPD -> thunk with the HLE opcode.
    CHECK(img.imports.size() == 2);
    CHECK(img.imports[0].name == "cellGcmSys" && img.imports[0].functions.size() == 2);
    CHECK(img.imports[1].name == "sys_fs" && img.imports[1].functions.size() == 1);
    for (const auto& m : img.imports) {
        for (const auto& f : m.functions) {
            const u32 opd = mem.Read<u32>(f.slot);
            const u32 code = mem.Read<u32>(opd);
            u32 index = 0;
            CHECK(hle::DecodeHleOpcode(mem.Read<u32>(code), &index) && index == f.hle_index);
            CHECK(mem.Read<u32>(code + 4) == hle::kPpcBlr);
            CHECK(mem.Flags(code) & vm::kPageCode);
            CHECK(hle.At(index).nid == f.nid && hle.At(index).module == m.name);
        }
    }
    // The registered implementation is reached through the import; others are stubs.
    const auto& gcm = img.imports[0].functions[0];
    CHECK(hle.At(gcm.hle_index).implemented);
    CHECK(!hle.At(img.imports[0].functions[1].hle_index).implemented);
    ppu::PpuThreadContext scratch{};
    g_hle_called = false;
    hle.Dispatch(gcm.hle_index, scratch);
    CHECK(g_hle_called && scratch.gpr[3] == 0x1234);
    hle.Dispatch(img.imports[1].functions[0].hle_index, scratch);  // stub: logs once, CELL_OK
    CHECK(scratch.gpr[3] == 0);

    // TLS block copied, r13 biased.
    CHECK(std::memcmp(mem.shadow() + img.tls_block + loader::kTlsTcbSize, "TLSDATA!", 8) == 0);
    CHECK(ctx.gpr[13] == img.tls_block + loader::kTlsTcbSize + loader::kTlsTpOffset);

    // Stack: guard, alignment, back chain, argv.
    CHECK(vm::GuestMemory::AreaOf(img.stack_base) == vm::Area::Stack);
    CHECK(!(mem.Flags(img.stack_base) & vm::kPageReadable));  // guard page
    CHECK(ctx.gpr[1] % 16 == 0 && mem.Read<u64>(static_cast<u32>(ctx.gpr[1])) == 0);
    CHECK(ctx.gpr[3] == 1);
    // argv[]/envp[]: 64-bit elements (lv2 layout), envp right after argv's NULL.
    const u32 argv = static_cast<u32>(ctx.gpr[4]);
    const u64 argv0 = mem.Read<u64>(argv);
    CHECK(argv0 >> 32 == 0);
    const auto arg0 = mem.ReadCString(static_cast<u32>(argv0), 256);
    CHECK(arg0 && *arg0 == "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN");
    CHECK(mem.Read<u64>(argv + 8) == 0);
    CHECK(ctx.gpr[5] == argv + 16 && mem.Read<u64>(argv + 16) == 0);
    // What the crt0 does next (regression for the MGS4 MAIN crash at 0x10710): narrow the
    // vector in place, ld slot i*8 -> stw slot i*4, then main reads argv[0] with lwz.
    for (u32 i = 0; i <= ctx.gpr[3]; ++i) mem.Write<u32>(argv + i * 4, static_cast<u32>(mem.Read<u64>(argv + i * 8)));
    const auto narrowed = mem.ReadCString(mem.Read<u32>(argv), 256);
    CHECK(narrowed && *narrowed == "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN");
    CHECK(mem.Read<u32>(argv + 4) == 0);

    // Registers handed to the crt0; LR returns into the exit thunk.
    CHECK(ctx.cia == kText && ctx.gpr[2] == kData + 0x8000);
    CHECK(ctx.gpr[7] == loader::kPrimaryThreadId && ctx.gpr[8] == kTls && ctx.gpr[9] == 8 && ctx.gpr[10] == 0x20);
    CHECK(ctx.gpr[11] == kOpd && ctx.gpr[12] == 0x100000);
    u32 exit_index = 0;
    CHECK(hle::DecodeHleOpcode(mem.Read<u32>(static_cast<u32>(ctx.lr)), &exit_index) &&
          exit_index == hle.exit_index());
}

void RunLoad(const std::vector<u8>& file, const char* label) {
    std::printf("-- load %s\n", label);
    vm::GuestMemory mem;
    mem.Initialize();
    hle::HleRegistry hle(mem);
    hle.Register("cellGcmSys", kNidGcmInit, "cellGcmInit (test)", FakeGcmInit);
    loader::PpuLoader ld(mem, hle);
    const auto exe = loader::ParseExecutable(file);
    const auto img = ld.Load(exe, {});
    ppu::PpuThreadContext ctx;
    ld.InitializePrimaryThread(img, ctx);
    CheckLoaded(mem, hle, img, ctx);
}

void TestRetailRejected() {
    std::printf("-- retail SELF is rejected\n");
    bool threw = false;
    try {
        (void)loader::ParseExecutable(BuildSelf(0x0004));
    } catch (const loader::LoadError& e) {
        threw = std::strstr(e.what(), "retail") != nullptr;
    }
    CHECK(threw);
}

void TestMalformed() {
    std::printf("-- malformed inputs\n");
    auto expect_error = [](std::vector<u8> file) {
        try {
            (void)loader::ParseExecutable(std::move(file));
        } catch (const loader::LoadError&) {
            return true;
        }
        return false;
    };
    auto elf = BuildElf();
    elf.resize(elf.size() - 1);  // truncated data segment
    CHECK(expect_error(elf));
    elf = BuildElf();
    elf[5] = 1;  // little-endian: a PS4 / x86 ELF
    CHECK(expect_error(elf));
    elf = BuildElf();
    Put16(elf, 0x12, 23);  // EM_SPU
    CHECK(expect_error(elf));
    std::vector<u8> junk(64, 0xAB);
    CHECK(expect_error(junk));
}

void TestSysMemory() {
    std::printf("-- sys_memory syscalls\n");
    vm::GuestMemory mem;
    mem.Initialize(8 * vm::k1M);  // tiny budget to exercise ENOMEM
    lv2::SyscallTable table;
    lv2::MemoryService svc(mem);
    svc.RegisterSyscalls(table);

    // Scratch out-parameter page for the guest.
    const auto out = mem.Allocate(vm::Area::Main, vm::k64K, vm::k64K, vm::kProtReadWrite);
    CHECK(out.has_value());

    ppu::PpuThreadContext ctx{};
    auto call = [&](u32 number, u64 a, u64 b, u64 c) {
        ctx.gpr[11] = number;
        ctx.gpr[3] = a;
        ctx.gpr[4] = b;
        ctx.gpr[5] = c;
        table.Dispatch(ctx);
        return static_cast<s32>(ctx.gpr[3]);
    };
    CHECK(call(lv2::SYS_MEMORY_ALLOCATE, 0x300000, lv2::SYS_MEMORY_PAGE_SIZE_1M, *out) == CELL_OK);
    const u32 a1 = mem.Read<u32>(*out);
    CHECK(vm::GuestMemory::AreaOf(a1) == vm::Area::User1M && a1 % vm::k1M == 0);
    CHECK(mem.Check(a1, 0x300000, vm::kPageWritable));

    // Guest-view write is visible in the shadow view (same backing pages).
    mem.base()[a1 + 0x10] = 0x5A;
    CHECK(mem.shadow()[a1 + 0x10] == 0x5A);

    CHECK(call(lv2::SYS_MEMORY_ALLOCATE, 0x20000, lv2::SYS_MEMORY_PAGE_SIZE_64K, *out) == CELL_OK);
    const u32 a2 = mem.Read<u32>(*out);
    CHECK(vm::GuestMemory::AreaOf(a2) == vm::Area::User64K);

    CHECK(call(lv2::SYS_MEMORY_ALLOCATE, 0x1000, lv2::SYS_MEMORY_PAGE_SIZE_1M, *out) == CELL_EALIGN);
    CHECK(call(lv2::SYS_MEMORY_ALLOCATE, 0x100000, 0x123, *out) == CELL_EINVAL);
    CHECK(call(lv2::SYS_MEMORY_ALLOCATE, 0x100000, lv2::SYS_MEMORY_PAGE_SIZE_1M, 0x8) == CELL_EFAULT);
    CHECK(call(lv2::SYS_MEMORY_ALLOCATE, 0x600000, lv2::SYS_MEMORY_PAGE_SIZE_1M, *out) == CELL_ENOMEM);  // over budget

    CHECK(call(lv2::SYS_MEMORY_GET_USER_MEMORY_SIZE, *out, 0, 0) == CELL_OK);
    CHECK(mem.Read<u32>(*out) == 8 * vm::k1M);
    CHECK(mem.Read<u32>(*out + 4) == 8 * vm::k1M - 0x300000 - 0x20000);

    CHECK(call(lv2::SYS_MEMORY_FREE, a1 + 0x1000, 0, 0) == CELL_EINVAL);  // not a base
    CHECK(call(lv2::SYS_MEMORY_FREE, a1, 0, 0) == CELL_OK);
    CHECK(!mem.Check(a1, 1, vm::kPageReadable));
    // Re-allocation hands back zeroed memory.
    CHECK(call(lv2::SYS_MEMORY_ALLOCATE, 0x300000, lv2::SYS_MEMORY_PAGE_SIZE_1M, *out) == CELL_OK);
    const u32 a3 = mem.Read<u32>(*out);
    CHECK(a3 == a1 && mem.shadow()[a3 + 0x10] == 0);

    ctx.gpr[11] = 999;  // unknown syscall
    table.Dispatch(ctx);
    CHECK(static_cast<s32>(ctx.gpr[3]) == CELL_ENOSYS);

    // Host fault address -> guest EA.
    CHECK(mem.GuestAddressOf(mem.base() + 0x12345678) == 0x12345678u);
    CHECK(!mem.GuestAddressOf(mem.shadow()).has_value() || mem.shadow() == mem.base());
}

}  // namespace

int main(int argc, char** argv) {
    log::Threshold() = log::Level::Warn;
    // `loader_tests --dump DIR` writes the fixtures so the CLI can be tried on them.
    if (argc == 3 && !std::strcmp(argv[1], "--dump")) {
        auto save = [&](const char* name, const std::vector<u8>& bytes) {
            const std::string path = std::string(argv[2]) + "/" + name;
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(bytes.data(), 1, bytes.size(), f);
                std::fclose(f);
            }
        };
        save("fixture_EBOOT.elf", BuildElf());
        save("fixture_EBOOT_debug.self", BuildSelf(loader::kKeyRevisionDebug));
        save("fixture_EBOOT_retail.self", BuildSelf(0x0004));
        return 0;
    }
    RunLoad(BuildElf(), "plain ELF");
    RunLoad(BuildSelf(loader::kKeyRevisionDebug), "debug SELF (zlib data segment)");
    TestRetailRejected();
    TestMalformed();
    TestSysMemory();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all loader tests passed\n");
    return 0;
}
