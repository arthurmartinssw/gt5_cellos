// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/sys_spu_hle.cpp
#include "hle/sys_spu_hle.h"

#include <algorithm>
#include <cstring>

#include "common/log.h"
#include "lv2/sys_ppu_thread.h"
#include "spu/spu_elf.h"

namespace cell::hle {

namespace {

using C = ppu::PpuThreadContext;
using M = vm::GuestMemory;

u32 Arg32(const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); }
void Return(C& c, s64 value) { c.gpr[3] = static_cast<u64>(value); }

constexpr u32 kSegmentBytes = 16;  // sys_spu_segment_t

void WriteSegments(M& m, u32 at, const std::vector<spu::SpuSegment>& segments, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const spu::SpuSegment& s = segments[i];
        const u32 e = at + static_cast<u32>(i) * kSegmentBytes;
        m.Write<u32>(e + 0, s.type);
        m.Write<u32>(e + 4, s.ls_start);
        m.Write<u32>(e + 8, s.size);
        m.Write<u32>(e + 12, s.addr_or_value);
    }
}

// sys_spu_image_t: be32 type; be32 entry_point; be32 segs; be32 nsegs
void sys_spu_image_import(C& c, M& m, const HleFunction& self) {
    const u32 img = Arg32(c, 0), elf = Arg32(c, 1), type = Arg32(c, 2);
    if (!m.Check(img, 16, vm::kPageWritable)) return Return(c, CELL_EFAULT);
    if (type != spu::SYS_SPU_IMAGE_PROTECT && type != spu::SYS_SPU_IMAGE_DIRECT) return Return(c, CELL_EINVAL);
    std::string error;
    auto info = spu::ParseSpuElf(m, elf, &error);
    if (!info) {
        LOG_ERROR("sysPrxForUser", "sys_spu_image_import(0x%08x): %s", elf, error.c_str());
        return Return(c, CELL_ENOEXEC);
    }
    const u32 table_size = AlignUp<u32>(static_cast<u32>(info->segments.size()) * kSegmentBytes, 128);

    // PROTECT: copy the span the COPY segments read from, right after the segment table.
    u32 lo = 0xFFFFFFFFu, hi = 0;
    if (type == spu::SYS_SPU_IMAGE_PROTECT)
        for (const auto& s : info->segments)
            if (s.type == spu::SYS_SPU_SEGMENT_TYPE_COPY) {
                lo = std::min(lo, s.addr_or_value);
                hi = std::max(hi, s.addr_or_value + s.size);
            }
    const u32 copy_size = hi > lo ? hi - lo : 0;
    if (copy_size && !m.Check(lo, copy_size, vm::kPageReadable)) return Return(c, CELL_EFAULT);
    const auto block = m.Allocate(vm::Area::Main, std::max<u32>(table_size + copy_size, 1), vm::k64K, vm::kProtReadWrite);
    if (!block) return Return(c, CELL_ENOMEM);
    if (copy_size) {
        std::memcpy(m.shadow() + *block + table_size, m.shadow() + lo, copy_size);
        for (auto& s : info->segments)
            if (s.type == spu::SYS_SPU_SEGMENT_TYPE_COPY) s.addr_or_value = s.addr_or_value - lo + *block + table_size;
    }
    WriteSegments(m, *block, info->segments, info->segments.size());
    m.Write<u32>(img + 0, spu::SYS_SPU_IMAGE_TYPE_USER);
    m.Write<u32>(img + 4, info->entry);
    m.Write<u32>(img + 8, *block);
    m.Write<u32>(img + 12, static_cast<u32>(info->segments.size()));
    LOG_INFO("sysPrxForUser", "sys_spu_image_import(ELF 0x%08x, %s): entry 0x%05x, %zu segments at 0x%08x",
             elf, type == spu::SYS_SPU_IMAGE_PROTECT ? "PROTECT" : "DIRECT", info->entry, info->segments.size(), *block);
    (void)self;
    Return(c, CELL_OK);
}

void sys_spu_image_close(C& c, M& m, const HleFunction&) {
    const u32 img = Arg32(c, 0);
    if (!m.Check(img, 16, vm::kPageWritable)) return Return(c, CELL_EFAULT);
    if (m.Read<u32>(img) != spu::SYS_SPU_IMAGE_TYPE_USER) return Return(c, CELL_EINVAL);
    if (!m.Free(vm::Area::Main, m.Read<u32>(img + 8))) return Return(c, CELL_EINVAL);
    m.Write<u32>(img + 8, 0);
    m.Write<u32>(img + 12, 0);
    Return(c, CELL_OK);
}

// (u32 elf, u32* entry, s32* nseg)
void sys_spu_elf_get_information(C& c, M& m, const HleFunction&) {
    const u32 entry = Arg32(c, 1), nseg = Arg32(c, 2);
    if (!m.Check(entry, 4, vm::kPageWritable) || !m.Check(nseg, 4, vm::kPageWritable)) return Return(c, CELL_EFAULT);
    std::string error;
    const auto info = spu::ParseSpuElf(m, Arg32(c, 0), &error);
    if (!info) return Return(c, CELL_ENOEXEC);
    m.Write<u32>(entry, info->entry);
    m.Write<u32>(nseg, static_cast<u32>(info->segments.size()));
    Return(c, CELL_OK);
}

// (u32 elf, sys_spu_segment_t* segments, s32 nseg)
void sys_spu_elf_get_segments(C& c, M& m, const HleFunction&) {
    const u32 out = Arg32(c, 1);
    const s32 nseg = static_cast<s32>(c.gpr[5]);
    std::string error;
    const auto info = spu::ParseSpuElf(m, Arg32(c, 0), &error);
    if (!info) return Return(c, CELL_ENOEXEC);
    if (nseg < 0) return Return(c, CELL_EINVAL);
    const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(nseg), info->segments.size());
    if (!m.Check(out, static_cast<u32>(count) * kSegmentBytes, vm::kPageWritable)) return Return(c, CELL_EFAULT);
    WriteSegments(m, out, info->segments, count);
    Return(c, CELL_OK);
}


SysSpuHle& Self(const HleFunction& f) { return *static_cast<SysSpuHle*>(f.user); }

void spu_printf_initialize(C& c, M&, const HleFunction& self) {
    SysSpuHle& s = Self(self);
    for (int i = 0; i < SysSpuHle::kCallbackCount; ++i) s.spu_printf[i].store(Arg32(c, i));
    s.spu_printf_initialized.store(true);
    LOG_INFO("sysPrxForUser", "_sys_spu_printf_initialize: callbacks attach_group 0x%08x detach_group 0x%08x "
             "attach_thread 0x%08x detach_thread 0x%08x", Arg32(c, 0), Arg32(c, 1), Arg32(c, 2), Arg32(c, 3));
    Return(c, CELL_OK);
}

void spu_printf_finalize(C& c, M&, const HleFunction& self) {
    SysSpuHle& s = Self(self);
    if (!s.spu_printf_initialized.exchange(false)) return Return(c, CELL_ESTAT);
    for (auto& cb : s.spu_printf) cb.store(0);
    Return(c, CELL_OK);
}

// Calls callback `which` with the group/thread id on a helper PPU thread (CallGuest) and
// returns what it returned. A null callback is accepted (the server did not ask for it).
template <SysSpuHle::Callback kWhich>
void spu_printf_dispatch(C& c, M&, const HleFunction& self) {
    static constexpr const char* kNames[] = {"spu_printf attach_group", "spu_printf detach_group",
                                             "spu_printf attach_thread", "spu_printf detach_thread"};
    SysSpuHle& s = Self(self);
    if (!s.spu_printf_initialized.load()) return Return(c, CELL_ESTAT);
    const u32 opd = s.spu_printf[kWhich].load(), id = Arg32(c, 0);
    if (!opd) return Return(c, CELL_OK);
    LOG_INFO("sysPrxForUser", "%s(0x%08x): calling guest callback 0x%08x", kNames[kWhich], id, opd);
    const auto r = s.kernel().threads().CallGuest(opd, id, 0, kNames[kWhich]);
    if (!r) return Return(c, CELL_ESRCH);
    Return(c, static_cast<s32>(*r));
}

void raw_spu_unsupported(C& c, M&, const HleFunction&) {
    c.exit_kind = ppu::kExitAbort;
    c.exit_message = "raw SPUs (sys_raw_spu_load) are not supported: only SPU thread groups are mapped to host threads";
    c.exit_status = 0;
    c.exit_requested = true;
}

}  // namespace

void SysSpuHle::Register(HleRegistry& r) {
    constexpr const char* kModule = "sysPrxForUser";
    struct Entry {
        const char* name;
        HleFunc fn;
    };
    static constexpr Entry kEntries[] = {
        {"sys_spu_image_import", sys_spu_image_import},
        {"sys_spu_image_close", sys_spu_image_close},
        {"sys_spu_elf_get_information", sys_spu_elf_get_information},
        {"sys_spu_elf_get_segments", sys_spu_elf_get_segments},
        // The exported names carry a leading underscore (the SDK's spu_printf_* macros call
        // them); the plain names are kept for titles linked against older stubs.
        {"_sys_spu_printf_initialize", spu_printf_initialize},
        {"_sys_spu_printf_finalize", spu_printf_finalize},
        {"_sys_spu_printf_attach_group", spu_printf_dispatch<SysSpuHle::kAttachGroup>},
        {"_sys_spu_printf_detach_group", spu_printf_dispatch<SysSpuHle::kDetachGroup>},
        {"_sys_spu_printf_attach_thread", spu_printf_dispatch<SysSpuHle::kAttachThread>},
        {"_sys_spu_printf_detach_thread", spu_printf_dispatch<SysSpuHle::kDetachThread>},
        {"sys_spu_printf_initialize", spu_printf_initialize},
        {"sys_spu_printf_finalize", spu_printf_finalize},
        {"sys_spu_printf_attach_group", spu_printf_dispatch<SysSpuHle::kAttachGroup>},
        {"sys_spu_printf_detach_group", spu_printf_dispatch<SysSpuHle::kDetachGroup>},
        {"sys_spu_printf_attach_thread", spu_printf_dispatch<SysSpuHle::kAttachThread>},
        {"sys_spu_printf_detach_thread", spu_printf_dispatch<SysSpuHle::kDetachThread>},
        {"sys_raw_spu_load", raw_spu_unsupported},
        {"sys_raw_spu_image_load", raw_spu_unsupported},
    };
    for (const Entry& e : kEntries) r.Register(kModule, e.name, e.fn, this);
}

}  // namespace cell::hle
