// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — loader/self.cpp
#include "loader/self.h"

#include <cinttypes>
#include <cstdio>
#include <fstream>

#include <zlib.h>

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

// Bounds-checked big-endian reader: the equivalent of prepare.py's span()/unpack(), which
// check every offset before use ("All offsets are checked before use").
class BeReader {
public:
    explicit BeReader(std::span<const u8> data) noexcept : data_(data) {}

    void Require(u64 offset, u64 size, const char* what) const {
        if (offset > data_.size() || size > data_.size() - offset)
            Fail("out of bounds: %s at 0x%" PRIx64 "+0x%" PRIx64 " (file is 0x%zx bytes)", what, offset, size,
                 data_.size());
    }
    template <typename T>
    [[nodiscard]] T Get(u64 offset, const char* what) const {
        Require(offset, sizeof(T), what);
        T value;
        std::memcpy(&value, data_.data() + offset, sizeof(T));
        return ByteSwap(value);
    }
    [[nodiscard]] std::span<const u8> Slice(u64 offset, u64 size, const char* what) const {
        Require(offset, size, what);
        return data_.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
    }

private:
    std::span<const u8> data_;
};

struct ElfHeader {
    u8 osabi;
    u16 type;
    u64 entry;
    u64 phoff;
    u16 phnum;
};

// ELF64 header, big-endian (Elf64_Ehdr). Offsets: e_type 0x10, e_machine 0x12, e_entry 0x18,
// e_phoff 0x20, e_phentsize 0x36, e_phnum 0x38.
ElfHeader ParseElfHeader(const BeReader& r, u64 at) {
    const auto ident = r.Slice(at, 16, "ELF e_ident");
    if (ident[0] != 0x7F || ident[1] != 'E' || ident[2] != 'L' || ident[3] != 'F')
        Fail("no ELF header at 0x%" PRIx64, at);
    if (ident[4] != 2) Fail("ELF class %u: PPU executables are ELF64", ident[4]);
    if (ident[5] != 2) Fail("ELF data encoding %u: expected big-endian (2); a PS4/x86 binary?", ident[5]);

    ElfHeader h{};
    h.osabi = ident[7];
    h.type = r.Get<u16>(at + 0x10, "e_type");
    const u16 machine = r.Get<u16>(at + 0x12, "e_machine");
    h.entry = r.Get<u64>(at + 0x18, "e_entry");
    h.phoff = r.Get<u64>(at + 0x20, "e_phoff");
    const u16 phentsize = r.Get<u16>(at + 0x36, "e_phentsize");
    h.phnum = r.Get<u16>(at + 0x38, "e_phnum");

    if (machine != EM_PPC64) Fail("e_machine %u is not PPC64 (21); SPU images (23) are loaded by the SPU runtime", machine);
    if (phentsize != 56) Fail("e_phentsize %u (expected 56)", phentsize);
    if (h.phnum == 0 || h.phnum > 256) Fail("e_phnum %u out of range", h.phnum);
    if (h.osabi != ELFOSABI_CELL_LV2)
        LOG_WARN("loader", "ELF OSABI 0x%02x is not CELL LV2 (0x66); loading anyway", h.osabi);
    return h;
}

// Elf64_Phdr, big-endian: p_type 0, p_flags 4, p_offset 8, p_vaddr 0x10, p_paddr 0x18,
// p_filesz 0x20, p_memsz 0x28, p_align 0x30.
ElfProgramHeader ParsePhdr(const BeReader& r, u64 at) {
    ElfProgramHeader p{};
    p.type = r.Get<u32>(at + 0x00, "p_type");
    p.flags = r.Get<u32>(at + 0x04, "p_flags");
    p.offset = r.Get<u64>(at + 0x08, "p_offset");
    p.vaddr = r.Get<u64>(at + 0x10, "p_vaddr");
    p.paddr = r.Get<u64>(at + 0x18, "p_paddr");
    p.filesz = r.Get<u64>(at + 0x20, "p_filesz");
    p.memsz = r.Get<u64>(at + 0x28, "p_memsz");
    p.align = r.Get<u64>(at + 0x30, "p_align");
    return p;
}

// Segments the loader reads through guest addresses. Only PT_LOAD needs file bytes: TLS,
// process param and PRX param live *inside* PT_LOAD segments and are read from guest
// memory after mapping (in a SELF their section-info entries may be empty).
bool IsAddressed(u32 type) noexcept {
    return type == PT_LOAD || type == PT_TLS || type == PT_PROC_PARAM || type == PT_PROC_PRX_PARAM;
}

void ValidatePhdr(const ElfProgramHeader& p, std::size_t index) {
    // Only mapped segments have a memory image; e.g. a PRX relocation table (0x700000A4) is
    // file-only and may carry p_memsz = 0.
    if (IsAddressed(p.type) && p.filesz > p.memsz)
        Fail("phdr %zu: p_filesz 0x%" PRIx64 " > p_memsz 0x%" PRIx64, index, p.filesz, p.memsz);
    if (IsAddressed(p.type) && p.memsz && (p.vaddr > 0xFFFFFFFFull || p.vaddr + p.memsz > 0x100000000ull))
        Fail("phdr %zu: [0x%" PRIx64 ", +0x%" PRIx64 ") leaves the 32-bit PS3 address space", index, p.vaddr, p.memsz);
}

std::vector<u8> Inflate(std::span<const u8> source, u64 expected, std::size_t index) {
    std::vector<u8> out(static_cast<std::size_t>(expected));
    uLongf length = static_cast<uLongf>(expected);
    const int rc = uncompress(out.data(), &length, source.data(), static_cast<uLong>(source.size()));
    if (rc != Z_OK || length != expected)
        Fail("phdr %zu: zlib inflate failed (rc=%d, got 0x%lx of 0x%" PRIx64 " bytes)", index, rc,
             static_cast<unsigned long>(length), expected);
    return out;
}

void ParsePlainElf(PpuExecutable& exe) {
    const BeReader r(exe.file);
    const ElfHeader h = ParseElfHeader(r, 0);
    exe.type = h.type;
    exe.osabi = h.osabi;
    exe.entry = h.entry;
    for (u16 i = 0; i < h.phnum; ++i) {
        const ElfProgramHeader p = ParsePhdr(r, h.phoff + u64{i} * 56);
        ValidatePhdr(p, i);
        exe.phdrs.push_back(p);
        // Like prepare.py: a missing PT_LOAD payload is fatal, other segments are optional.
        // PRX relocations are not inside a PT_LOAD: keep their bytes too.
        if ((p.type == PT_LOAD || p.type == PT_SCE_PPURELA) && p.filesz)
            exe.payload.push_back(r.Slice(p.offset, p.filesz, p.type == PT_LOAD ? "PT_LOAD payload" : "relocation table"));
        else exe.payload.emplace_back();
    }
}

void ParseSelf(PpuExecutable& exe) {
    const BeReader r(exe.file);
    // ---- SCE header (0x20 bytes) ----
    const u32 version = r.Get<u32>(0x04, "SCE version");
    const u16 key_revision = r.Get<u16>(0x08, "SCE key revision");
    const u16 header_type = r.Get<u16>(0x0A, "SCE header type");
    if (version != 2) Fail("SCE header version %u (PS3 SELFs use 2)", version);
    if (header_type != kSceHeaderTypeSelf) Fail("SCE file category %u is not a SELF", header_type);

    // ---- extended SELF header (0x50 bytes at 0x20) ----
    const u64 app_info_offset = r.Get<u64>(0x28, "app info offset");
    const u64 elf_offset = r.Get<u64>(0x30, "ELF offset");
    const u64 phdr_offset = r.Get<u64>(0x38, "phdr offset");
    const u64 section_info_offset = r.Get<u64>(0x48, "section info offset");

    // ---- app info (0x20 bytes): auth_id, vendor_id, self_type, version ----
    exe.self.wrapped = true;
    exe.self.key_revision = key_revision;
    exe.self.auth_id = r.Get<u64>(app_info_offset, "auth id");
    exe.self.self_type = r.Get<u32>(app_info_offset + 0x0C, "self type");
    exe.self.app_version = r.Get<u64>(app_info_offset + 0x10, "app version");

    // ---- embedded ELF header and phdrs: stored in plaintext even in retail SELFs ----
    const ElfHeader h = ParseElfHeader(r, elf_offset);
    exe.type = h.type;
    exe.osabi = h.osabi;
    exe.entry = h.entry;

    // Retail SELFs: metadata (keys/IVs for each section) is encrypted with per-revision keys
    // this project does not ship. Same stance as prepare.py ("No decryption"): ask for a
    // plaintext dump instead of guessing.
    if (key_revision != kKeyRevisionDebug)
        Fail("retail SELF (key revision 0x%04x, self type %u): segments are encrypted. Decrypt your own dump "
             "of EBOOT.BIN to a plain ELF first (e.g. RPCS3: Utilities > Decrypt PS3 Binaries) and load that",
             key_revision, exe.self.self_type);

    for (u16 i = 0; i < h.phnum; ++i) {
        const ElfProgramHeader p = ParsePhdr(r, phdr_offset + u64{i} * 56);
        ValidatePhdr(p, i);
        exe.phdrs.push_back(p);
        if ((p.type != PT_LOAD && p.type != PT_SCE_PPURELA) || !p.filesz) {
            exe.payload.emplace_back();
            continue;
        }
        // Section info entry i (0x20 bytes): offset, size, compressed (1 no / 2 zlib),
        // two unknown words, encrypted (1 yes / 2 no). One entry per phdr, same order —
        // unlike PS4 SELF entries, which carry the phdr index in flags bits 20+.
        const u64 si = section_info_offset + u64{i} * 0x20;
        const u64 data_offset = r.Get<u64>(si + 0x00, "section offset");
        const u64 data_size = r.Get<u64>(si + 0x08, "section size");
        const u32 compressed = r.Get<u32>(si + 0x10, "section compressed");
        const u32 encrypted = r.Get<u32>(si + 0x1C, "section encrypted");
        if (encrypted == 1) Fail("PT_LOAD phdr %u is marked encrypted in this debug SELF", i);
        if (compressed == 2) {
            exe.inflated.push_back(Inflate(r.Slice(data_offset, data_size, "compressed segment"), p.filesz, i));
            exe.payload.emplace_back(exe.inflated.back());
        } else {
            if (data_size < p.filesz) Fail("phdr %u: section size 0x%" PRIx64 " < p_filesz", i, data_size);
            exe.payload.push_back(r.Slice(data_offset, p.filesz, "segment payload"));
        }
    }
}

}  // namespace

std::vector<u8> ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw LoadError("cannot open " + path.string());
    const auto size = static_cast<std::streamoff>(in.tellg());
    if (size <= 0 || size > (std::streamoff{1} << 30)) throw LoadError("unexpected file size for " + path.string());
    std::vector<u8> data(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(data.data()), size)) throw LoadError("short read on " + path.string());
    return data;
}

namespace {
PpuExecutable ParseElfImage(std::vector<u8> file) {
    PpuExecutable exe;
    exe.file = std::move(file);
    exe.inflated.reserve(256);  // payload spans point into these; no reallocation of inner buffers
    const BeReader r(exe.file);
    const u32 magic = r.Get<u32>(0, "file magic");
    if (magic == 0x7F454C46u) ParsePlainElf(exe);
    else if (magic == kSceMagic) ParseSelf(exe);
    else Fail("file magic 0x%08x: neither a PS3 SELF (SCE\\0) nor an ELF", magic);
    return exe;
}
}  // namespace

PpuExecutable ParseExecutable(std::vector<u8> file) {
    PpuExecutable exe = ParseElfImage(std::move(file));
    if (exe.type == ET_SCE_PPURELEXEC) Fail("this is a PRX (e_type 0xFFA4); the main executable loader takes EBOOT.BIN");
    if (exe.type != ET_EXEC) Fail("e_type 0x%04x is not ET_EXEC", exe.type);
    return exe;
}

PpuExecutable ParsePrx(std::vector<u8> file) {
    PpuExecutable exe = ParseElfImage(std::move(file));
    if (exe.type != ET_SCE_PPURELEXEC) Fail("e_type 0x%04x is not a PRX (0xFFA4)", exe.type);
    return exe;
}

}  // namespace cell::loader
