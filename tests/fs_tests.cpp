// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/fs_tests.cpp
//
// lv2 file system + cellFs HLE, on real host files in a temporary directory:
//   * host::File: 64-bit positional I/O on a sparse 5 GiB file, data straddling 4 GiB;
//   * a disc layout (/dev_bdvd, read-only) with a sparse 5 GiB "GT.VOL": open, fstat,
//     64-bit lseek, sequential reads, cellFsReadWithOffset leaving the position alone, EOF,
//     concurrent ReadWithOffset from several host threads on one fd;
//   * read-only device: every write path answers EROFS;
//   * /dev_hdd0 (writable): create/trunc/append/excl, stat, mkdir/opendir/readdir (layout of
//     CellFsDirent), rename, unlink, rmdir, truncate;
//   * errors: ENOENT (and the "missing" report), ENOTMOUNTED, EBADF, EINVAL, EMFILE, ".." ;
//   * case-insensitive fallback for dumps with different case (Linux hosts);
//   * the sys_fs_* syscalls reach the same service (801/802/818/804).
#include <atomic>
#include <cstring>
#include <filesystem>
#include <thread>

#include "common/host_file.h"
#include "hle/cell_fs.h"
#include "hle/fnid.h"
#include "lv2/sys_fs.h"
#include "test_machine.h"

using namespace cell;
namespace fs = std::filesystem;

namespace {

constexpr u64 kGiB = 1ull << 30;
constexpr u64 kVolSize = 5 * kGiB + 123;
constexpr u64 kStraddle = 4 * kGiB - 0x2000;  // 16 KiB pattern across the 4 GiB boundary
constexpr u32 kPatternSize = 0x4000;

u8 Pattern(u64 offset) { return static_cast<u8>((offset * 2654435761ull) >> 13); }

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("cellos_fs_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Sparse file of `size` bytes with the pattern at kStraddle and a marker in the last 16 bytes.
void MakeVolume(const fs::path& p, u64 size) {
    host::FileError e{};
    auto f = host::File::Open(p, host::kFileWrite | host::kFileCreate | host::kFileTruncate, &e);
    CHECK(f.has_value());
    if (!f) return;
    CHECK(f->Truncate(size) == host::FileError::None);
    std::vector<u8> buf(kPatternSize);
    for (u32 i = 0; i < kPatternSize; ++i) buf[i] = Pattern(kStraddle + i);
    u64 done = 0;
    CHECK(f->WriteAt(kStraddle, buf.data(), buf.size(), &done) == host::FileError::None);
    CHECK_EQ(done, kPatternSize);
    const char tail[] = "END-OF-VOLUME!!";
    CHECK(f->WriteAt(size - 16, tail, 16, &done) == host::FileError::None);
}

void WriteText(const fs::path& p, const char* text) {
    host::FileError e{};
    auto f = host::File::Open(p, host::kFileWrite | host::kFileCreate | host::kFileTruncate, &e);
    u64 done = 0;
    if (f) f->WriteAt(0, text, std::strlen(text), &done);
}

struct FsSetup {
    TempDir dir;
    Machine m;
    hle::CellFs cell_fs{*m.kernel};
    fs::path disc, hdd;

    FsSetup() {
        cell_fs.Register(*m.hle);
        disc = dir.path / "disc";
        hdd = dir.path / "hdd0";
        fs::create_directories(disc / "PS3_GAME" / "USRDIR");
        fs::create_directories(hdd);
        MakeVolume(disc / "PS3_GAME" / "USRDIR" / "GT.VOL", kVolSize);
        WriteText(disc / "PS3_GAME" / "USRDIR" / "small.txt", "hello disc");
        m.kernel->vfs().Mount("/dev_bdvd", disc, true);
        m.kernel->vfs().Mount("/dev_hdd0", hdd, false);
    }

    u32 Str(const char* s, u32 at = kData + 0x8000) {
        m.memory.CopyIn(at, s, std::strlen(s) + 1);
        return at;
    }
    s32 Call(const char* name, std::initializer_list<u64> args) {
        const u32 index = m.hle->Bind("sys_fs", hle::ComputeFnid(name));
        const hle::HleFunction& f = m.hle->At(index);
        ppu::PpuThreadContext c{};
        int r = 3;
        for (u64 a : args) c.gpr[r++] = a;
        f.fn(c, m.memory, f);
        return static_cast<s32>(c.gpr[3]);
    }
    s32 Sys(u32 n, std::initializer_list<u64> args) {
        ppu::PpuThreadContext c{};
        c.gpr[11] = n;
        int r = 3;
        for (u64 a : args) c.gpr[r++] = a;
        m.syscalls.Dispatch(c);
        return static_cast<s32>(c.gpr[3]);
    }
    // cellFsOpen; returns fd or the (negative) error.
    s32 Open(const char* path, s32 flags) {
        const s32 e = Call("cellFsOpen", {Str(path), static_cast<u64>(flags), kData + 0x10, 0, 0});
        return e == CELL_OK ? static_cast<s32>(m.memory.Read<u32>(kData + 0x10)) : e;
    }
    u64 U64(u32 ea) { return m.memory.Read<u64>(ea); }
};

constexpr u32 kOut = kData + 0x20, kBuf = kData + 0x1000;

void TestHostFile() {
    std::printf("-- host::File: 64-bit positional I/O on a sparse 5 GiB file\n");
    TempDir dir;
    const fs::path p = dir.path / "vol.bin";
    MakeVolume(p, kVolSize);
    host::FileError e{};
    auto f = host::File::Open(p, host::kFileRead, &e);
    CHECK(f.has_value());
    if (!f) return;
    CHECK_EQ(f->Size().value_or(0), kVolSize);
    std::vector<u8> buf(kPatternSize);
    u64 done = 0;
    CHECK(f->ReadAt(kStraddle, buf.data(), buf.size(), &done) == host::FileError::None);
    CHECK_EQ(done, kPatternSize);
    bool same = true;
    for (u32 i = 0; i < kPatternSize; ++i) same &= buf[i] == Pattern(kStraddle + i);
    CHECK(same);
    CHECK(f->ReadAt(kVolSize - 4, buf.data(), 100, &done) == host::FileError::None);  // short at EOF
    CHECK_EQ(done, 4);
    CHECK(std::memcmp(buf.data(), "E!!", 4) == 0);  // last 4 bytes of "END-OF-VOLUME!!\0"
    CHECK(f->ReadAt(kVolSize + 10, buf.data(), 100, &done) == host::FileError::None);
    CHECK_EQ(done, 0);
    CHECK(!host::File::Open(dir.path / "nope", host::kFileRead, &e).has_value());
    CHECK(e == host::FileError::NotFound);
    CHECK(!host::File::Open(p, host::kFileWrite | host::kFileCreate | host::kFileExclusive, &e).has_value());
    CHECK(e == host::FileError::Exists);
    CHECK(!host::File::Open(dir.path, host::kFileRead, &e).has_value());
    CHECK(e == host::FileError::IsDir || e == host::FileError::Access);
}

void TestDiscVolume() {
    std::printf("-- /dev_bdvd volume: open, fstat, 64-bit lseek, read, ReadWithOffset, EOF\n");
    FsSetup s;
    const s32 fd = s.Open("/dev_bdvd/PS3_GAME/USRDIR/GT.VOL", lv2::CELL_FS_O_RDONLY);
    CHECK(fd >= 3 && fd < 256);
    // fstat: CellFsStat is 52 bytes, 64-bit fields 4-byte aligned; size at +36
    s.m.memory.Fill(kOut, 0xEE, 64);
    CHECK_EQ(s.Call("cellFsFstat", {static_cast<u64>(fd), kOut}), CELL_OK);
    CHECK_EQ(s.m.memory.Read<u32>(kOut), static_cast<u32>(lv2::CELL_FS_S_IFREG | 0444));
    CHECK_EQ(s.m.memory.Read<u64>(kOut + 36), kVolSize);
    CHECK_EQ(s.m.memory.Read<u64>(kOut + 44), 2048);  // Blu-ray block
    CHECK_EQ(s.m.memory.Read<u8>(kOut + 52), 0xEE);   // nothing past the struct

    // lseek to 4 GiB - 0x2000 (needs all 64 bits), then a sequential read across 4 GiB
    CHECK_EQ(s.Call("cellFsLseek", {static_cast<u64>(fd), kStraddle, lv2::CELL_FS_SEEK_SET, kOut}), CELL_OK);
    CHECK_EQ(s.U64(kOut), kStraddle);
    CHECK_EQ(s.Call("cellFsRead", {static_cast<u64>(fd), kBuf, kPatternSize, kOut}), CELL_OK);
    CHECK_EQ(s.U64(kOut), kPatternSize);
    bool same = true;
    for (u32 i = 0; i < kPatternSize; ++i) same &= s.m.memory.Read<u8>(kBuf + i) == Pattern(kStraddle + i);
    CHECK(same);
    CHECK_EQ(s.Call("cellFsLseek", {static_cast<u64>(fd), 0, lv2::CELL_FS_SEEK_CUR, kOut}), CELL_OK);
    CHECK_EQ(s.U64(kOut), kStraddle + kPatternSize);

    // ReadWithOffset: reads the tail, position unchanged
    CHECK_EQ(s.Call("cellFsReadWithOffset", {static_cast<u64>(fd), kVolSize - 16, kBuf, 16, kOut}), CELL_OK);
    CHECK_EQ(s.U64(kOut), 16);
    CHECK(s.m.memory.ReadCString(kBuf, 16).value_or("") == "END-OF-VOLUME!!");
    CHECK_EQ(s.Call("cellFsLseek", {static_cast<u64>(fd), 0, lv2::CELL_FS_SEEK_CUR, kOut}), CELL_OK);
    CHECK_EQ(s.U64(kOut), kStraddle + kPatternSize);

    // SEEK_END, short read at EOF, read past EOF, null nread allowed
    CHECK_EQ(s.Call("cellFsLseek", {static_cast<u64>(fd), static_cast<u64>(-4), lv2::CELL_FS_SEEK_END, kOut}), CELL_OK);
    CHECK_EQ(s.U64(kOut), kVolSize - 4);
    CHECK_EQ(s.Call("cellFsRead", {static_cast<u64>(fd), kBuf, 0x1000, kOut}), CELL_OK);
    CHECK_EQ(s.U64(kOut), 4);
    CHECK_EQ(s.Call("cellFsRead", {static_cast<u64>(fd), kBuf, 0x1000, kOut}), CELL_OK);
    CHECK_EQ(s.U64(kOut), 0);
    CHECK_EQ(s.Call("cellFsRead", {static_cast<u64>(fd), kBuf, 0x10, 0}), CELL_OK);
    CHECK_EQ(s.Call("cellFsLseek", {static_cast<u64>(fd), static_cast<u64>(-1), lv2::CELL_FS_SEEK_SET, kOut}), CELL_EINVAL);
    CHECK_EQ(s.Call("cellFsLseek", {static_cast<u64>(fd), 0, 7, kOut}), CELL_EINVAL);
    // the buffer must be writable guest memory
    CHECK_EQ(s.Call("cellFsReadWithOffset", {static_cast<u64>(fd), 0, 0xB0000000, 0x100, kOut}), CELL_EFAULT);
    // writes on a read-only fd
    CHECK_EQ(s.Call("cellFsWrite", {static_cast<u64>(fd), kBuf, 4, kOut}), CELL_EBADF);

    // concurrent ReadWithOffset on the same fd from 4 host threads (as several PPU threads would)
    std::atomic<int> bad{0};
    std::vector<std::thread> readers;
    for (u32 t = 0; t < 4; ++t)
        readers.emplace_back([&, t] {
            const u32 buf = kData + 0x6000 + t * 0x800, out = kData + 0x40 + t * 8;
            for (u32 i = 0; i < 64; ++i) {
                const u64 off = kStraddle + ((t * 64 + i) * 37u) % (kPatternSize - 0x400);
                if (s.m.kernel->fs().GuestReadAt(static_cast<u32>(fd), off, buf, 0x400, out) != CELL_OK ||
                    s.m.memory.Read<u64>(out) != 0x400) {
                    ++bad;
                    continue;
                }
                for (u32 k = 0; k < 0x400; k += 97)
                    if (s.m.memory.Read<u8>(buf + k) != Pattern(off + k)) ++bad;
            }
        });
    for (auto& r : readers) r.join();
    CHECK_EQ(bad.load(), 0);

    CHECK_EQ(s.Call("cellFsClose", {static_cast<u64>(fd)}), CELL_OK);
    CHECK_EQ(s.Call("cellFsClose", {static_cast<u64>(fd)}), CELL_EBADF);
    CHECK_EQ(s.Call("cellFsRead", {static_cast<u64>(fd), kBuf, 4, kOut}), CELL_EBADF);
}

void TestReadOnlyDevice() {
    std::printf("-- read-only /dev_bdvd: write paths answer EROFS\n");
    FsSetup s;
    CHECK_EQ(s.Open("/dev_bdvd/PS3_GAME/USRDIR/small.txt", lv2::CELL_FS_O_RDWR), CELL_EROFS);
    CHECK_EQ(s.Open("/dev_bdvd/new.bin", lv2::CELL_FS_O_WRONLY | lv2::CELL_FS_O_CREAT), CELL_EROFS);
    CHECK_EQ(s.Call("cellFsUnlink", {s.Str("/dev_bdvd/PS3_GAME/USRDIR/small.txt")}), CELL_EROFS);
    CHECK_EQ(s.Call("cellFsMkdir", {s.Str("/dev_bdvd/x"), 0777}), CELL_EROFS);
    CHECK_EQ(s.Call("cellFsTruncate", {s.Str("/dev_bdvd/PS3_GAME/USRDIR/small.txt"), 0}), CELL_EROFS);
    CHECK(fs::file_size(s.disc / "PS3_GAME" / "USRDIR" / "small.txt") == 10);
    CHECK_EQ(s.Call("cellFsGetFreeSize", {s.Str("/dev_bdvd"), kOut, kOut + 8}), CELL_OK);
    CHECK_EQ(s.m.memory.Read<u32>(kOut), 2048);
    CHECK_EQ(s.U64(kOut + 8), 0);
}

void TestWritableDevice() {
    std::printf("-- /dev_hdd0: create/trunc/append/excl, stat, dirs, rename, unlink\n");
    FsSetup s;
    using namespace lv2;
    const s32 fd = s.Open("/dev_hdd0/save.dat", CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC);
    CHECK(fd >= 3);
    s.Str("0123456789", kBuf);
    CHECK_EQ(s.Call("cellFsWrite", {static_cast<u64>(fd), kBuf, 10, kOut}), CELL_OK);
    CHECK_EQ(s.U64(kOut), 10);
    CHECK_EQ(s.Call("cellFsRead", {static_cast<u64>(fd), kBuf, 4, kOut}), CELL_EBADF);  // write-only
    CHECK_EQ(s.Call("cellFsClose", {static_cast<u64>(fd)}), CELL_OK);
    CHECK_EQ(s.Open("/dev_hdd0/save.dat", CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_EXCL), CELL_EEXIST);

    const s32 fa = s.Open("/dev_hdd0/save.dat", CELL_FS_O_RDWR | CELL_FS_O_APPEND);
    s.Str("AB", kBuf);
    CHECK_EQ(s.Call("cellFsWrite", {static_cast<u64>(fa), kBuf, 2, kOut}), CELL_OK);
    CHECK_EQ(s.Call("cellFsReadWithOffset", {static_cast<u64>(fa), 8, kBuf, 16, kOut}), CELL_OK);
    CHECK_EQ(s.U64(kOut), 4);
    CHECK(std::memcmp(s.m.memory.shadow() + kBuf, "89AB", 4) == 0);
    CHECK_EQ(s.Call("cellFsFtruncate", {static_cast<u64>(fa), 3}), CELL_OK);
    CHECK_EQ(s.Call("cellFsFsync", {static_cast<u64>(fa)}), CELL_OK);
    CHECK_EQ(s.Call("cellFsClose", {static_cast<u64>(fa)}), CELL_OK);
    CHECK_EQ(s.Call("cellFsStat", {s.Str("/dev_hdd0/save.dat"), kOut}), CELL_OK);
    CHECK_EQ(s.m.memory.Read<u32>(kOut), static_cast<u32>(CELL_FS_S_IFREG | 0666));
    CHECK_EQ(s.m.memory.Read<u64>(kOut + 36), 3);
    CHECK(s.m.memory.Read<u64>(kOut + 20) > 1600000000ull);  // mtime: a plausible Unix time

    // directories
    CHECK_EQ(s.Call("cellFsMkdir", {s.Str("/dev_hdd0/game"), 0777}), CELL_OK);
    CHECK_EQ(s.Call("cellFsMkdir", {s.Str("/dev_hdd0/game"), 0777}), CELL_EEXIST);
    CHECK_EQ(s.Call("cellFsMkdir", {s.Str("/dev_hdd0/a/b"), 0777}), CELL_ENOENT);
    CHECK_EQ(s.Call("cellFsStat", {s.Str("/dev_hdd0/game"), kOut}), CELL_OK);
    CHECK_EQ(s.m.memory.Read<u32>(kOut), static_cast<u32>(CELL_FS_S_IFDIR | 0777));
    CHECK_EQ(s.Call("cellFsRename", {s.Str("/dev_hdd0/save.dat"), s.Str("/dev_hdd0/game/SAVE.DAT", kData + 0x8400)}), CELL_OK);
    CHECK_EQ(s.Call("cellFsRename", {s.Str("/dev_hdd0/game/SAVE.DAT"), s.Str("/dev_bdvd/x", kData + 0x8400)}), CELL_EXDEV);
    WriteText(s.hdd / "game" / "B.BIN", "b");
    CHECK_EQ(s.Call("cellFsOpendir", {s.Str("/dev_hdd0/game"), kOut}), CELL_OK);
    const u32 dfd = s.m.memory.Read<u32>(kOut);
    std::vector<std::pair<u8, std::string>> names;
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(s.Call("cellFsReaddir", {dfd, kBuf, kOut}), CELL_OK);
        if (s.U64(kOut) == 0) break;
        CHECK_EQ(s.U64(kOut), kFsDirentSize);
        const u8 len = s.m.memory.Read<u8>(kBuf + 1);
        names.emplace_back(s.m.memory.Read<u8>(kBuf), std::string(reinterpret_cast<const char*>(s.m.memory.shadow() + kBuf + 2), len));
    }
    CHECK_EQ(names.size(), 4);
    if (names.size() == 4) {
        CHECK(names[0].second == "." && names[1].second == "..");
        CHECK(names[2].second == "B.BIN" && names[2].first == CELL_FS_TYPE_REGULAR);
        CHECK(names[3].second == "SAVE.DAT");
    }
    CHECK_EQ(s.Call("cellFsClose", {dfd}), CELL_EBADF);  // a directory fd is closed with closedir
    CHECK_EQ(s.Call("cellFsClosedir", {dfd}), CELL_OK);
    CHECK_EQ(s.Call("cellFsRmdir", {s.Str("/dev_hdd0/game")}), CELL_ENOTEMPTY);
    CHECK_EQ(s.Call("cellFsUnlink", {s.Str("/dev_hdd0/game")}), CELL_EISDIR);
    CHECK_EQ(s.Call("cellFsUnlink", {s.Str("/dev_hdd0/game/SAVE.DAT")}), CELL_OK);
    CHECK_EQ(s.Call("cellFsUnlink", {s.Str("/dev_hdd0/game/B.BIN")}), CELL_OK);
    CHECK_EQ(s.Call("cellFsUnlink", {s.Str("/dev_hdd0/game/B.BIN")}), CELL_ENOENT);
    CHECK_EQ(s.Call("cellFsRmdir", {s.Str("/dev_hdd0/game")}), CELL_OK);
    CHECK(!fs::exists(s.hdd / "game"));

    // the device root lists the mounts
    CHECK_EQ(s.Call("cellFsOpendir", {s.Str("/"), kOut}), CELL_OK);
    const u32 root = s.m.memory.Read<u32>(kOut);
    std::string all;
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(s.Call("cellFsReaddir", {root, kBuf, kOut}), CELL_OK);
        if (!s.U64(kOut)) break;
        all += s.m.memory.ReadCString(kBuf + 2, 256).value_or("") + ";";
    }
    CHECK(all == ".;..;dev_bdvd;dev_hdd0;");
    CHECK_EQ(s.Call("cellFsClosedir", {root}), CELL_OK);
}

void TestErrors() {
    std::printf("-- errors: ENOENT (+ missing report), ENOTMOUNTED, EINVAL, EMFILE, '..', case fallback\n");
    FsSetup s;
    using namespace lv2;
    CHECK_EQ(s.Open("/dev_bdvd/PS3_GAME/USRDIR/NOPE.VOL", CELL_FS_O_RDONLY), CELL_ENOENT);
    bool listed = false;
    for (const std::string& l : s.m.kernel->fs().Describe()) listed |= l.find("missing: \"/dev_bdvd/PS3_GAME/USRDIR/NOPE.VOL\"") != std::string::npos;
    CHECK(listed);
    CHECK_EQ(s.Open("/dev_usb000/a", CELL_FS_O_RDONLY), CELL_ENOTMOUNTED);
    CHECK_EQ(s.Open("relative/path", CELL_FS_O_RDONLY), CELL_ENOENT);
    CHECK_EQ(s.Open("/dev_hdd0/../escape", CELL_FS_O_RDONLY), CELL_ENOENT);
    CHECK_EQ(s.Open("/dev_hdd0/x", 3), CELL_EINVAL);
    CHECK_EQ(s.Open("/dev_bdvd/PS3_GAME/USRDIR", CELL_FS_O_RDONLY), CELL_EISDIR);
    CHECK_EQ(s.Call("cellFsOpen", {s.Str("/dev_bdvd/PS3_GAME/USRDIR/small.txt"), 0, 0, 0, 0}), CELL_EFAULT);  // no fd out
    CHECK_EQ(s.Call("cellFsOpen", {0xB0000000, 0, kOut, 0, 0}), CELL_EFAULT);
    CHECK_EQ(s.Call("cellFsStat", {s.Str("/dev_hdd0/none"), kOut}), CELL_ENOENT);
    CHECK_EQ(s.Call("cellFsOpendir", {s.Str("/dev_bdvd/PS3_GAME/USRDIR/small.txt"), kOut}), CELL_ENOTDIR);
#ifndef _WIN32
    // dumps whose case differs from what the game asks for
    const s32 fd = s.Open("/dev_bdvd/ps3_game/usrdir/SMALL.TXT", CELL_FS_O_RDONLY);
    CHECK(fd >= 3);
    if (fd >= 3) CHECK_EQ(s.Call("cellFsClose", {static_cast<u64>(fd)}), CELL_OK);
#endif
    // fds 3..255
    std::vector<s32> fds;
    for (;;) {
        const s32 fd2 = s.Open("/dev_bdvd/PS3_GAME/USRDIR/small.txt", CELL_FS_O_RDONLY);
        if (fd2 < 0) {
            CHECK_EQ(fd2, CELL_EMFILE);
            break;
        }
        fds.push_back(fd2);
        if (fds.size() > 400) break;
    }
    CHECK_EQ(fds.size(), 253);
    for (s32 fd2 : fds) s.Call("cellFsClose", {static_cast<u64>(fd2)});
}

void TestSyscalls() {
    std::printf("-- sys_fs_* syscalls reach the same service\n");
    FsSetup s;
    CHECK_EQ(s.Sys(801, {s.Str("/dev_bdvd/PS3_GAME/USRDIR/small.txt"), 0, kOut, 0, 0, 0}), CELL_OK);
    const u32 fd = s.m.memory.Read<u32>(kOut);
    CHECK_EQ(s.Sys(818, {fd, 6, lv2::CELL_FS_SEEK_SET, kOut}), CELL_OK);
    CHECK_EQ(s.Sys(802, {fd, kBuf, 100, kOut}), CELL_OK);
    CHECK_EQ(s.U64(kOut), 4);
    CHECK(std::memcmp(s.m.memory.shadow() + kBuf, "disc", 4) == 0);
    CHECK_EQ(s.Sys(809, {fd, kOut}), CELL_OK);
    CHECK_EQ(s.m.memory.Read<u64>(kOut + 36), 10);
    CHECK_EQ(s.Sys(804, {fd}), CELL_OK);
    CHECK_EQ(s.Sys(804, {fd}), CELL_EBADF);
    CHECK_EQ(s.Sys(808, {s.Str("/dev_bdvd/PS3_GAME"), kOut}), CELL_OK);
    CHECK_EQ(s.m.memory.Read<u32>(kOut), static_cast<u32>(lv2::CELL_FS_S_IFDIR | 0555));
}

}  // namespace

int main() {
    log::Threshold() = log::Level::Error;
    TestHostFile();
    TestDiscVolume();
    TestReadOnlyDevice();
    TestWritableDevice();
    TestErrors();
    TestSyscalls();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all file system tests passed\n");
    return 0;
}
