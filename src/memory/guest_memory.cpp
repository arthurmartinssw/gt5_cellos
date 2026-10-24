// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — memory/guest_memory.cpp
#include "memory/guest_memory.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "common/log.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/falloc.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace cell::vm {

// =======================================================================================
// GranuleAllocator
// =======================================================================================

void GranuleAllocator::Init(u32 base, u64 size, u32 granule) {
    base_ = base;
    size_ = size;
    granule_ = granule;
    shift_ = static_cast<u32>(std::countr_zero(granule));
    count_ = static_cast<u32>(size >> shift_);
    bitmap_.assign((count_ + 63) / 64, 0);
    blocks_.clear();
    used_ = 0;
}

void GranuleAllocator::SetRange(u32 first, u32 count, bool used) noexcept {
    for (u32 i = first; i < first + count; ++i) {
        const u64 bit = u64{1} << (i & 63);
        if (used) bitmap_[i >> 6] |= bit;
        else bitmap_[i >> 6] &= ~bit;
    }
}

std::optional<u32> GranuleAllocator::Allocate(u64 size, u32 align) {
    if (!size || size > size_) return std::nullopt;
    const u32 count = static_cast<u32>((size + granule_ - 1) >> shift_);
    // Alignments at or below one granule are free. Larger ones are applied to absolute
    // addresses, not indices: the area base itself may be unaligned (main starts at 0x10000).
    auto first_aligned = [&](u32 index) -> u32 {
        const u64 addr = u64{base_} + (u64{index} << shift_);
        const u64 aligned = AlignUp<u64>(addr, std::max<u64>(align, granule_));
        return static_cast<u32>((aligned - base_) >> shift_);
    };
    for (u32 i = first_aligned(0); i + count <= count_;) {
        // Skip fully used 64-granule words quickly.
        if ((i & 63) == 0 && bitmap_[i >> 6] == ~u64{0}) {
            i = first_aligned(i + 64);
            continue;
        }
        u32 j = 0;
        while (j < count && !IsUsed(i + j)) ++j;
        if (j == count) {
            SetRange(i, count, true);
            blocks_.emplace(i, count);
            used_ += u64{count} << shift_;
            return base_ + (i << shift_);
        }
        i = first_aligned(i + j + 1);  // restart past the used granule
    }
    return std::nullopt;
}

bool GranuleAllocator::AllocateFixed(u32 addr, u64 size) {
    if (addr < base_ || (addr - base_) & (granule_ - 1) || !size) return false;
    const u64 offset = addr - base_;
    if (offset + size > size_) return false;
    const u32 first = static_cast<u32>(offset >> shift_);
    const u32 count = static_cast<u32>((size + granule_ - 1) >> shift_);
    for (u32 i = first; i < first + count; ++i)
        if (IsUsed(i)) return false;
    SetRange(first, count, true);
    blocks_.emplace(first, count);
    used_ += u64{count} << shift_;
    return true;
}

u64 GranuleAllocator::Free(u32 addr) {
    if (addr < base_) return 0;
    const auto it = blocks_.find(static_cast<u32>((addr - base_) >> shift_));
    if (it == blocks_.end() || base_ + (it->first << shift_) != addr) return 0;
    const u64 bytes = u64{it->second} << shift_;
    SetRange(it->first, it->second, false);
    blocks_.erase(it);
    used_ -= bytes;
    return bytes;
}

u64 GranuleAllocator::BlockSize(u32 addr) const {
    if (addr < base_) return 0;
    const auto it = blocks_.find(static_cast<u32>((addr - base_) >> shift_));
    return it == blocks_.end() ? 0 : u64{it->second} << shift_;
}

// =======================================================================================
// GuestMemory — platform layer
// =======================================================================================

namespace {
u8 FlagsFor(u8 prot) noexcept {
    return kPageMapped | ((prot & kProtRead) ? kPageReadable : 0) |
           ((prot & kProtWrite) ? kPageWritable | kPageReadable : 0);
}
#if defined(_WIN32)
DWORD HostProtection(u8 prot) noexcept {
    if (prot & kProtWrite) return PAGE_READWRITE;
    if (prot & kProtRead) return PAGE_READONLY;
    return PAGE_NOACCESS;
}
std::string LastError(const char* what) {
    return std::string(what) + " failed (GetLastError=" + std::to_string(GetLastError()) + ")";
}
// Placeholder APIs (Windows 10 1803+) resolved at run time from kernelbase.dll, so the build
// needs neither onecore.lib nor a recent NTDDI_VERSION, and works the same on MSVC and MinGW.
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
struct PlaceholderApi {
    VirtualAlloc2Fn VirtualAlloc2 = nullptr;
    MapViewOfFile3Fn MapViewOfFile3 = nullptr;
};
PlaceholderApi LoadPlaceholderApi() {
    PlaceholderApi api;
    if (HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll")) {
        api.VirtualAlloc2 = reinterpret_cast<VirtualAlloc2Fn>(
            reinterpret_cast<void*>(GetProcAddress(kernelbase, "VirtualAlloc2")));
        api.MapViewOfFile3 = reinterpret_cast<MapViewOfFile3Fn>(
            reinterpret_cast<void*>(GetProcAddress(kernelbase, "MapViewOfFile3")));
    }
    return api;
}
#else
int HostProtection(u8 prot) noexcept {
    if (prot & kProtWrite) return PROT_READ | PROT_WRITE;
    if (prot & kProtRead) return PROT_READ;
    return PROT_NONE;
}
std::string LastError(const char* what) {
    return std::string(what) + " failed: " + std::strerror(errno);
}
#endif
}  // namespace

GuestMemory::GuestMemory() = default;
GuestMemory::~GuestMemory() { Shutdown(); }

void GuestMemory::Initialize(u64 user_memory_budget) {
    if (base_) throw std::runtime_error("guest memory already initialized");
    user_budget_ = user_memory_budget;

#if defined(_WIN32)
    // Same idea as bloodborne_pc's bb_reserve_arena + bb_section_create: a pagefile-backed
    // section the arena is a view of. SEC_RESERVE keeps all 4 GiB reserved, not committed;
    // VirtualAlloc(MEM_COMMIT) on the view commits section pages on demand, and the
    // shadow view sees them because both views share the section.
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
                                        static_cast<DWORD>(kGuestSpaceSize >> 32),
                                        static_cast<DWORD>(kGuestSpaceSize & 0xFFFFFFFFu), nullptr);
    if (!section) throw std::runtime_error(LastError("CreateFileMappingW(4 GiB, SEC_RESERVE)"));
    // One placeholder for arena + guard, split so the arena part can be replaced by the
    // view while the guard stays an inaccessible placeholder (Windows 10 1803+).
    const PlaceholderApi api = LoadPlaceholderApi();
    if (!api.VirtualAlloc2 || !api.MapViewOfFile3) {
        CloseHandle(section);
        throw std::runtime_error("VirtualAlloc2/MapViewOfFile3 not found: Windows 10 version 1803 or later is required");
    }
    void* placeholder = api.VirtualAlloc2(GetCurrentProcess(), nullptr, kGuestSpaceSize + kGuardSize,
                                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (!placeholder) {
        CloseHandle(section);
        throw std::runtime_error(LastError("VirtualAlloc2(placeholder)"));
    }
    if (!VirtualFree(placeholder, kGuestSpaceSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        VirtualFree(placeholder, 0, MEM_RELEASE);
        CloseHandle(section);
        throw std::runtime_error(LastError("VirtualFree(split placeholder)"));
    }
    void* view = api.MapViewOfFile3(section, GetCurrentProcess(), placeholder, 0, kGuestSpaceSize,
                                    MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    void* shadow = view ? api.MapViewOfFile3(section, GetCurrentProcess(), nullptr, 0, kGuestSpaceSize, 0,
                                             PAGE_READWRITE, nullptr, 0)
                        : nullptr;
    if (!view || !shadow) {
        const std::string error = LastError("MapViewOfFile3(arena)");
        if (view) UnmapViewOfFile(view);
        else VirtualFree(placeholder, 0, MEM_RELEASE);
        VirtualFree(static_cast<u8*>(placeholder) + kGuestSpaceSize, 0, MEM_RELEASE);
        CloseHandle(section);
        throw std::runtime_error(error);
    }
    section_ = section;
    guard_ = static_cast<u8*>(placeholder) + kGuestSpaceSize;
    base_ = static_cast<u8*>(view);
    shadow_ = static_cast<u8*>(shadow);
#else
    // Same idea as runtime_memory.c's pool(): a sparse memfd. Here file offset == EA, the
    // guest view is mapped PROT_NONE and opened page range by page range with mprotect,
    // and released ranges are hole-punched so the RAM goes back to the host.
    fd_ = memfd_create("cellos-guest-memory", MFD_CLOEXEC);
    if (fd_ < 0) throw std::runtime_error(LastError("memfd_create"));
    if (ftruncate(fd_, static_cast<off_t>(kGuestSpaceSize))) {
        const std::string error = LastError("ftruncate(4 GiB)");
        Shutdown();
        throw std::runtime_error(error);
    }
    // Reserve arena + guard in one go so nothing else can land right after 0xFFFFFFFF.
    void* reservation = mmap(nullptr, kGuestSpaceSize + kGuardSize, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reservation == MAP_FAILED) {
        const std::string error = LastError("mmap(reserve 4 GiB + guard)");
        Shutdown();
        throw std::runtime_error(error);
    }
    reservation_ = static_cast<u8*>(reservation);
    void* view = mmap(reservation, kGuestSpaceSize, PROT_NONE, MAP_SHARED | MAP_FIXED, fd_, 0);
    void* shadow = mmap(nullptr, kGuestSpaceSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, fd_, 0);
    if (view == MAP_FAILED || shadow == MAP_FAILED) {
        const std::string error = LastError("mmap(arena views)");
        if (shadow != MAP_FAILED) munmap(shadow, kGuestSpaceSize);
        Shutdown();
        throw std::runtime_error(error);
    }
    base_ = static_cast<u8*>(view);
    shadow_ = static_cast<u8*>(shadow);
#endif

    pages_ = std::make_unique<std::atomic<u8>[]>(kPageCount);  // value-initialized: all 0
    for (const AreaDesc& area : kLayout)
        allocators_[static_cast<std::size_t>(area.area)].Init(area.base, area.size, area.granule);

    g_fastmem_base = base_;
    LOG_INFO("vm", "guest arena: base=%p shadow=%p (4 GiB + %llu KiB guard), user budget %llu MiB",
             static_cast<void*>(base_), static_cast<void*>(shadow_),
             static_cast<unsigned long long>(kGuardSize >> 10),
             static_cast<unsigned long long>(user_budget_ >> 20));
}

void GuestMemory::Shutdown() noexcept {
    if (g_fastmem_base == base_) g_fastmem_base = nullptr;
#if defined(_WIN32)
    if (shadow_) UnmapViewOfFile(shadow_);
    if (base_) UnmapViewOfFile(base_);
    if (guard_) VirtualFree(guard_, 0, MEM_RELEASE);
    if (section_) CloseHandle(static_cast<HANDLE>(section_));
    section_ = guard_ = nullptr;
#else
    if (shadow_) munmap(shadow_, kGuestSpaceSize);
    if (reservation_) munmap(reservation_, kGuestSpaceSize + kGuardSize);
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    reservation_ = nullptr;
#endif
    base_ = shadow_ = nullptr;
}

bool GuestMemory::HostCommit(u32 addr, u64 size, u8 prot) noexcept {
#if defined(_WIN32)
    if (!VirtualAlloc(base_ + addr, size, MEM_COMMIT, PAGE_READWRITE)) return false;
    DWORD old;
    return HostProtection(prot) == PAGE_READWRITE ||
           VirtualProtect(base_ + addr, size, HostProtection(prot), &old);
#else
    // Pages of a sparse memfd are allocated on first touch: "commit" is just access.
    return mprotect(base_ + addr, size, HostProtection(prot)) == 0;
#endif
}

void GuestMemory::HostRelease(u32 addr, u64 size) noexcept {
#if defined(_WIN32)
    // Views of a section cannot be decommitted page by page. Zero through the shadow (lv2
    // hands out zeroed memory) and fence the guest view off. Commit charge is kept until
    // the range is reused; games allocate their big heaps once, so this is bounded.
    std::memset(shadow_ + addr, 0, size);
    DWORD old;
    VirtualProtect(base_ + addr, size, PAGE_NOACCESS, &old);
#else
    mprotect(base_ + addr, size, PROT_NONE);
    // Like zero_phys() in runtime_memory.c: the punched range reads back as zeros.
    fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(addr),
              static_cast<off_t>(size));
#endif
}

bool GuestMemory::HostProtect(u32 addr, u64 size, u8 prot) noexcept {
#if defined(_WIN32)
    DWORD old;
    return VirtualProtect(base_ + addr, size, HostProtection(prot), &old) != 0;
#else
    return mprotect(base_ + addr, size, HostProtection(prot)) == 0;
#endif
}

// =======================================================================================
// GuestMemory — allocation API
// =======================================================================================

void GuestMemory::SetFlags(u32 addr, u64 size, u8 flags, bool keep_code) noexcept {
    const u64 first = addr >> kPageShift;
    const u64 last = (u64{addr} + size - 1) >> kPageShift;
    for (u64 p = first; p <= last; ++p) {
        u8 value = flags;
        if (keep_code) value |= pages_[p].load(std::memory_order_relaxed) & kPageCode;
        pages_[p].store(value, std::memory_order_release);
    }
}

void GuestMemory::NotifyIfCode(u32 addr, u64 size) noexcept {
    if (!invalidate_hook_) return;
    const u64 first = addr >> kPageShift;
    const u64 last = (u64{addr} + size - 1) >> kPageShift;
    for (u64 p = first; p <= last; ++p) {
        if (pages_[p].load(std::memory_order_relaxed) & kPageCode) {
            invalidate_hook_(invalidate_user_, addr, size);
            return;
        }
    }
}

std::optional<u32> GuestMemory::Allocate(Area area, u64 size, u32 align, u8 prot) {
    std::lock_guard lock(mutex_);
    auto& allocator = allocators_[static_cast<std::size_t>(area)];
    const auto addr = allocator.Allocate(size, align);
    if (!addr) return std::nullopt;
    const u64 bytes = allocator.BlockSize(*addr);
    if (!HostCommit(*addr, bytes, prot)) {
        allocator.Free(*addr);
        LOG_ERROR("vm", "host commit of 0x%08x+0x%llx failed", *addr, static_cast<unsigned long long>(bytes));
        return std::nullopt;
    }
    SetFlags(*addr, bytes, FlagsFor(prot), false);
    return addr;
}

bool GuestMemory::AllocateFixed(Area area, u32 addr, u64 size, u8 prot) {
    std::lock_guard lock(mutex_);
    auto& allocator = allocators_[static_cast<std::size_t>(area)];
    if (!allocator.AllocateFixed(addr, size)) return false;
    const u64 bytes = allocator.BlockSize(addr);
    if (!HostCommit(addr, bytes, prot)) {
        allocator.Free(addr);
        return false;
    }
    SetFlags(addr, bytes, FlagsFor(prot), false);
    return true;
}

u64 GuestMemory::Free(Area area, u32 addr) {
    std::lock_guard lock(mutex_);
    auto& allocator = allocators_[static_cast<std::size_t>(area)];
    const u64 bytes = allocator.BlockSize(addr);
    if (!bytes || allocator.Free(addr) != bytes) return 0;
    NotifyIfCode(addr, bytes);  // translated blocks in the range die with it
    SetFlags(addr, bytes, 0, false);
    HostRelease(addr, bytes);
    return bytes;
}

std::optional<u32> GuestMemory::ReserveRange(Area area, u64 size, u32 align) {
    std::lock_guard lock(mutex_);
    return allocators_[static_cast<std::size_t>(area)].Allocate(size, align);
}

bool GuestMemory::ReserveFixedRange(Area area, u32 addr, u64 size) {
    std::lock_guard lock(mutex_);
    return allocators_[static_cast<std::size_t>(area)].AllocateFixed(addr, size);
}

bool GuestMemory::Unreserve(Area area, u32 addr) {
    std::lock_guard lock(mutex_);
    auto& allocator = allocators_[static_cast<std::size_t>(area)];
    const u64 bytes = allocator.BlockSize(addr);
    return bytes && allocator.Free(addr) == bytes;
}

bool GuestMemory::CommitRange(u32 addr, u64 size, u8 prot) {
    if (!size || (addr & (kPageSize - 1)) || (size & (kPageSize - 1)) || u64{addr} + size > kGuestSpaceSize)
        return false;
    std::lock_guard lock(mutex_);
    if (!HostCommit(addr, size, prot)) return false;
    SetFlags(addr, size, FlagsFor(prot), false);
    return true;
}

void GuestMemory::DecommitRange(u32 addr, u64 size) {
    std::lock_guard lock(mutex_);
    NotifyIfCode(addr, size);
    SetFlags(addr, size, 0, false);
    HostRelease(addr, size);
}

bool GuestMemory::Protect(u32 addr, u64 size, u8 prot) {
    if (!size || (addr & (kPageSize - 1)) || u64{addr} + size > kGuestSpaceSize) return false;
    size = AlignUp<u64>(size, kPageSize);
    std::lock_guard lock(mutex_);
    if (!Check(addr, size, kPageMapped)) return false;
    if (!HostProtect(addr, size, prot)) return false;
    SetFlags(addr, size, FlagsFor(prot), true);
    return true;
}

void GuestMemory::MarkCode(u32 addr, u64 size) noexcept {
    const u64 first = addr >> kPageShift;
    const u64 last = (u64{addr} + size - 1) >> kPageShift;
    for (u64 p = first; p <= last; ++p) pages_[p].fetch_or(kPageCode, std::memory_order_release);
}

void GuestMemory::SetInvalidateHook(InvalidateHook hook, void* user) noexcept {
    std::lock_guard lock(mutex_);
    invalidate_hook_ = hook;
    invalidate_user_ = user;
}

// =======================================================================================
// GuestMemory — queries
// =======================================================================================

bool GuestMemory::Check(u32 addr, u64 size, u8 required) const noexcept {
    if (!size) return true;
    if (u64{addr} + size > kGuestSpaceSize) return false;
    required |= kPageMapped;
    const u64 first = addr >> kPageShift;
    const u64 last = (u64{addr} + size - 1) >> kPageShift;
    for (u64 p = first; p <= last; ++p)
        if ((pages_[p].load(std::memory_order_acquire) & required) != required) return false;
    return true;
}

std::optional<u32> GuestMemory::GuestAddressOf(const void* host) const noexcept {
    const auto* p = static_cast<const u8*>(host);
    if (!base_ || p < base_ || p >= base_ + kGuestSpaceSize + kGuardSize) return std::nullopt;
    const u64 offset = static_cast<u64>(p - base_);
    // An access that faulted in the guard came from an EA near 0xFFFFFFFF wrapping.
    return static_cast<u32>(offset & 0xFFFFFFFFu);
}

std::optional<Area> GuestMemory::AreaOf(u32 addr) noexcept {
    for (const AreaDesc& area : kLayout)
        if (addr >= area.base && u64{addr} < u64{area.base} + area.size) return area.area;
    return std::nullopt;
}

std::optional<std::string> GuestMemory::ReadCString(u32 ea, std::size_t max) const {
    std::string out;
    for (std::size_t i = 0; i < max; ++i) {
        const u64 at = u64{ea} + i;
        if (at >= kGuestSpaceSize) return std::nullopt;
        if (i == 0 || (at & (kPageSize - 1)) == 0)
            if (!Check(static_cast<u32>(at), 1, kPageReadable)) return std::nullopt;
        const char c = static_cast<char>(shadow_[at]);
        if (!c) return out;
        out.push_back(c);
    }
    return std::nullopt;
}

bool GuestMemory::Charge(u64 bytes) noexcept {
    u64 used = user_used_.load(std::memory_order_relaxed);
    do {
        if (bytes > user_budget_ - used) return false;
    } while (!user_used_.compare_exchange_weak(used, used + bytes, std::memory_order_acq_rel));
    return true;
}

void GuestMemory::Uncharge(u64 bytes) noexcept {
    user_used_.fetch_sub(bytes, std::memory_order_acq_rel);
}

}  // namespace cell::vm
