// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/cell_fs.cpp
#include "hle/cell_fs.h"

#include "common/log.h"
#include "lv2/sys_fs.h"

namespace cell::hle {

namespace {

using C = ppu::PpuThreadContext;
using M = vm::GuestMemory;

u32 A32(const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); }
u64 A64(const C& c, int i) { return c.gpr[3 + i]; }
void Return(C& c, s32 value) { c.gpr[3] = static_cast<u64>(static_cast<s64>(value)); }
lv2::FsService& Fs(const HleFunction& f) { return static_cast<CellFs*>(f.user)->kernel().fs(); }

void cellFsOpen(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestOpen(A32(c, 0), static_cast<s32>(c.gpr[4]), A32(c, 2))); }
void cellFsRead(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestRead(A32(c, 0), A32(c, 1), A64(c, 2), A32(c, 3))); }
void cellFsWrite(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestWrite(A32(c, 0), A32(c, 1), A64(c, 2), A32(c, 3))); }
void cellFsClose(C& c, M&, const HleFunction& f) { Return(c, Fs(f).Close(A32(c, 0))); }
void cellFsLseek(C& c, M&, const HleFunction& f) {
    Return(c, Fs(f).GuestSeek(A32(c, 0), static_cast<s64>(A64(c, 1)), static_cast<s32>(c.gpr[5]), A32(c, 3)));
}
void cellFsReadWithOffset(C& c, M&, const HleFunction& f) {
    Return(c, Fs(f).GuestReadAt(A32(c, 0), A64(c, 1), A32(c, 2), A64(c, 3), A32(c, 4)));
}
void cellFsWriteWithOffset(C& c, M&, const HleFunction& f) {
    Return(c, Fs(f).GuestWriteAt(A32(c, 0), A64(c, 1), A32(c, 2), A64(c, 3), A32(c, 4)));
}
void cellFsStat(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestStat(A32(c, 0), A32(c, 1))); }
void cellFsFstat(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestFstat(A32(c, 0), A32(c, 1))); }
void cellFsOpendir(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestOpenDir(A32(c, 0), A32(c, 1))); }
void cellFsReaddir(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestReadDir(A32(c, 0), A32(c, 1), A32(c, 2))); }
void cellFsClosedir(C& c, M&, const HleFunction& f) { Return(c, Fs(f).CloseDir(A32(c, 0))); }
void cellFsMkdir(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestPathOp(A32(c, 0), &lv2::FsService::Mkdir)); }
void cellFsRmdir(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestPathOp(A32(c, 0), &lv2::FsService::Rmdir)); }
void cellFsUnlink(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestPathOp(A32(c, 0), &lv2::FsService::Unlink)); }
void cellFsRename(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestRename(A32(c, 0), A32(c, 1))); }
void cellFsTruncate(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestTruncate(A32(c, 0), A64(c, 1))); }
void cellFsFtruncate(C& c, M&, const HleFunction& f) { Return(c, Fs(f).Ftruncate(A32(c, 0), A64(c, 1))); }
void cellFsFsync(C& c, M&, const HleFunction& f) { Return(c, Fs(f).Fsync(A32(c, 0))); }
void cellFsGetBlockSize(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestGetBlockSize(A32(c, 0), A32(c, 1), A32(c, 2))); }
void cellFsFGetBlockSize(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestFGetBlockSize(A32(c, 0), A32(c, 1), A32(c, 2))); }
void cellFsGetFreeSize(C& c, M&, const HleFunction& f) { Return(c, Fs(f).GuestGetFreeSize(A32(c, 0), A32(c, 1), A32(c, 2))); }

// chmod/utime: succeed when the path exists (host permissions and times are not mapped).
void path_exists_ok(C& c, M& m, const HleFunction& f) {
    const auto path = m.ReadCString(A32(c, 0), lv2::kFsMaxPath + 1);
    if (!path) return Return(c, CELL_EFAULT);
    lv2::FsStat st;
    Return(c, Fs(f).Stat(*path, &st));
}

void accept(C& c, M&, const HleFunction& f) {
    LOG_TRACE("cellFs", "%s: accepted (no I/O buffer needed)", f.name.c_str());
    Return(c, CELL_OK);
}

}  // namespace

void CellFs::Register(HleRegistry& r) {
    constexpr const char* kModule = "sys_fs";
    struct Entry {
        const char* name;
        HleFunc fn;
    };
    static constexpr Entry kEntries[] = {
        {"cellFsOpen", cellFsOpen},
        {"cellFsRead", cellFsRead},
        {"cellFsWrite", cellFsWrite},
        {"cellFsClose", cellFsClose},
        {"cellFsLseek", cellFsLseek},
        {"cellFsReadWithOffset", cellFsReadWithOffset},
        {"cellFsWriteWithOffset", cellFsWriteWithOffset},
        {"cellFsStat", cellFsStat},
        {"cellFsFstat", cellFsFstat},
        {"cellFsOpendir", cellFsOpendir},
        {"cellFsReaddir", cellFsReaddir},
        {"cellFsClosedir", cellFsClosedir},
        {"cellFsMkdir", cellFsMkdir},
        {"cellFsRmdir", cellFsRmdir},
        {"cellFsUnlink", cellFsUnlink},
        {"cellFsRename", cellFsRename},
        {"cellFsTruncate", cellFsTruncate},
        {"cellFsFtruncate", cellFsFtruncate},
        {"cellFsFsync", cellFsFsync},
        {"cellFsGetBlockSize", cellFsGetBlockSize},
        {"cellFsFGetBlockSize", cellFsFGetBlockSize},
        {"cellFsGetFreeSize", cellFsGetFreeSize},
        {"cellFsChmod", path_exists_ok},
        {"cellFsUtime", path_exists_ok},
        {"cellFsSetDefaultContainer", accept},
        {"cellFsSetIoBuffer", accept},
        {"cellFsSetIoBufferFromDefaultContainer", accept},
    };
    for (const Entry& e : kEntries) r.Register(kModule, e.name, e.fn, this);
}

}  // namespace cell::hle
