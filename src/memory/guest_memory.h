// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — memory/guest_memory.h
//
// The PS3 guest address space: one 4 GiB host arena addressed by 32-bit effective
// addresses (EA), carved into the fixed lv2 areas.
//
// Mapping from bloodborne_pc/src/runtime_memory.c:
//
//   PS4 (runtime_memory.c)                         PS3 (this file)
//   ---------------------------------------------  ----------------------------------------
//   guest range [USER_MIN 0x10_0000_0000,          one 4 GiB arena, EA 0x00000000-0xFFFFFFFF
//     USER_MAX 0xFC_0000_0000), host addr == guest  host = base + EA (EA is 32-bit everywhere;
//     addr (code runs natively)                      the JIT pins base in a register)
//   memfd "bb-guest-memory" (Windows: a pagefile-  memfd "cellos-guest-memory" (Windows: a
//     backed section), sparse, mapped MAP_SHARED     SEC_RESERVE pagefile section), sparse,
//                                                    file offset == EA
//   backing_base: second RW view of the pool that  shadow(): second RW view of the arena that
//     host code writes through, bypassing guest      the loader / HLE / SPU DMA / RSX use,
//     and GPU page protection                        bypassing guest protection and SMC traps
//   PAGE 16 KiB                                     4 KiB tracking granule; lv2 allocation
//                                                    granules are 64 KiB and 1 MiB
//   direct memory (sceKernelAllocateDirectMemory)   sys_memory_allocate (64K / 1M pages),
//     + flexible memory (sceKernelMapFlexible...)    sys_mmapper (256 MiB address areas)
//   VMA table + rwlock                              fixed areas + per-area granule bitmaps
//                                                    + lock-free per-4K page flag array
//   hook_invalidate (GPU)                           code-invalidation hook (PPU/SPU JIT SMC)
//   bb_reserve_arena (Windows placeholder)          VirtualAlloc2 placeholder + MapViewOfFile3
//
// Hot-path contract for the PPU JIT: guest load/store == [g_fastmem_base + zext32(EA)].
// An unmapped or protected EA faults on the host; the fault handler calls GuestAddressOf()
// to turn the host address back into an EA and raise a guest data-storage exception, or,
// for a write to a kPageCode page, invalidates the translated block and resumes.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"

namespace cell::vm {

inline constexpr u64 kGuestSpaceSize = 0x1'0000'0000ull;  // 32-bit EAs
inline constexpr u64 kGuardSize = 0x10000;                // past 0xFFFFFFFF: catches wide straddling accesses
inline constexpr u32 kPageShift = 12;
inline constexpr u32 kPageSize = 1u << kPageShift;        // smallest PPU page, tracking granule
inline constexpr u32 kPageCount = static_cast<u32>(kGuestSpaceSize >> kPageShift);
inline constexpr u32 k64K = 0x10000;
inline constexpr u32 k1M = 0x100000;
inline constexpr u32 k256M = 0x10000000;

// Default for sys_memory_get_user_memory_size: 213 MiB of the 256 MiB XDR are left to a game.
inline constexpr u64 kDefaultUserMemory = 0x0D500000;

enum Prot : u8 { kProtNone = 0, kProtRead = 1, kProtWrite = 2, kProtReadWrite = 3 };

// Per-4K page state, readable without a lock (JIT, fault handler, SPU DMA validation).
enum PageFlag : u8 {
    kPageMapped = 0x01,
    kPageReadable = 0x02,
    kPageWritable = 0x04,
    kPageCode = 0x08,  // PPU/SPU JIT translated (or may translate) code here: writes invalidate
};

// lv2 address areas. Layout follows lv2 as reimplemented by RPCS3 (rpcs3/Emu/Memory/vm.cpp,
// Cell/lv2/sys_memory.cpp, sys_mmapper.cpp). Kept as data so a wrong boundary is a one-line
// fix: verify each row against RPCS3 when a game's first allocation lands somewhere odd.
enum class Area : u8 { Main, User64K, User1M, Mmapper, RsxLocal, Stack, RawSpu, SpuThread, Count };
inline constexpr std::size_t kAreaCount = static_cast<std::size_t>(Area::Count);

struct AreaDesc {
    Area area;
    const char* name;
    u32 base;
    u64 size;     // u64: the SPU thread area ends exactly at 4 GiB
    u32 granule;  // allocation unit inside the area
};

inline constexpr std::array<AreaDesc, kAreaCount> kLayout{{
    // EBOOT PT_LOAD segments (ET_EXEC at fixed vaddr, normally 0x10000), PRX images,
    // TLS blocks and the HLE thunk table. 0x0-0xFFFF stays unmapped: null-pointer trap.
    {Area::Main, "main", 0x00010000u, 0x1FFF0000ull, k64K},
    // sys_memory_allocate(SYS_MEMORY_PAGE_SIZE_64K)
    {Area::User64K, "user64k", 0x20000000u, 0x10000000ull, k64K},
    // sys_memory_allocate(SYS_MEMORY_PAGE_SIZE_1M) — where the game's heaps normally live
    {Area::User1M, "user1m", 0x30000000u, 0x10000000ull, k1M},
    // sys_mmapper_allocate_address: 256 MiB-aligned address areas for shared memory
    {Area::Mmapper, "mmapper", 0x40000000u, 0x80000000ull, k256M},
    // RSX local memory (256 MiB GDDR3), mapped by cellGcmInit; later imported into Vulkan
    {Area::RsxLocal, "rsx_local", 0xC0000000u, 0x10000000ull, k1M},
    // PPU thread stacks (primary + sys_ppu_thread_create)
    {Area::Stack, "stack", 0xD0000000u, 0x10000000ull, k64K},
    // Raw SPU MMIO: 1 MiB window per raw SPU (LS + problem-state registers)
    {Area::RawSpu, "raw_spu", 0xE0000000u, 0x10000000ull, k1M},
    // SPU thread group LS mapping: 1 MiB window per SPU thread; the SPU JIT uses
    // base + EA directly as its local-store pointer, so no extra alias is needed
    {Area::SpuThread, "spu_thread", 0xF0000000u, 0x10000000ull, k1M},
}};

[[nodiscard]] constexpr const AreaDesc& Describe(Area area) noexcept {
    return kLayout[static_cast<std::size_t>(area)];
}

// Bitmap allocator over one area. First-fit with word skipping: an area has at most
// 8192 granules (main, 64 KiB), so a full scan is a few hundred 64-bit tests.
class GranuleAllocator {
public:
    void Init(u32 base, u64 size, u32 granule);
    [[nodiscard]] std::optional<u32> Allocate(u64 size, u32 align);
    [[nodiscard]] bool AllocateFixed(u32 addr, u64 size);
    u64 Free(u32 addr);  // bytes released; 0 when addr is not the base of a block
    [[nodiscard]] u64 BlockSize(u32 addr) const;
    [[nodiscard]] u64 used_bytes() const noexcept { return used_; }
    [[nodiscard]] u32 granule() const noexcept { return granule_; }

private:
    [[nodiscard]] bool IsUsed(u32 index) const noexcept {
        return (bitmap_[index >> 6] >> (index & 63)) & 1u;
    }
    void SetRange(u32 first, u32 count, bool used) noexcept;

    u32 base_ = 0;
    u64 size_ = 0;
    u32 granule_ = 0;
    u32 shift_ = 0;
    u32 count_ = 0;
    std::vector<u64> bitmap_;
    std::unordered_map<u32, u32> blocks_;  // first granule -> granule count
    u64 used_ = 0;
};

class GuestMemory {
public:
    using InvalidateHook = void (*)(void* user, u32 addr, u64 size);

    GuestMemory();
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // Reserves the arena and both views. Throws std::runtime_error on failure.
    void Initialize(u64 user_memory_budget = kDefaultUserMemory);
    [[nodiscard]] bool initialized() const noexcept { return base_ != nullptr; }

    [[nodiscard]] u8* base() const noexcept { return base_; }
    [[nodiscard]] u8* shadow() const noexcept { return shadow_; }

    // ---- allocation (commit + page flags); thread safe ----
    [[nodiscard]] std::optional<u32> Allocate(Area area, u64 size, u32 align, u8 prot);
    [[nodiscard]] bool AllocateFixed(Area area, u32 addr, u64 size, u8 prot);
    u64 Free(Area area, u32 addr);
    bool Protect(u32 addr, u64 size, u8 prot);

    // ---- two-step model for sys_mmapper (address area first, memory mapped into it later) ----
    // Reserves granules of `area` without committing anything: the range stays inaccessible.
    [[nodiscard]] std::optional<u32> ReserveRange(Area area, u64 size, u32 align);
    [[nodiscard]] bool ReserveFixedRange(Area area, u32 addr, u64 size);
    // Releases a reservation. The caller guarantees nothing inside is committed any more.
    bool Unreserve(Area area, u32 addr);
    // Commits [addr, addr+size) inside a reservation with `prot` (page aligned). The new pages
    // read as zero.
    [[nodiscard]] bool CommitRange(u32 addr, u64 size, u8 prot);
    // Decommits a committed range (contents are lost; translated code inside is invalidated).
    void DecommitRange(u32 addr, u64 size);
    void MarkCode(u32 addr, u64 size) noexcept;
    void SetInvalidateHook(InvalidateHook hook, void* user) noexcept;

    // ---- lock-free queries ----
    [[nodiscard]] bool Check(u32 addr, u64 size, u8 required) const noexcept;
    [[nodiscard]] u8 Flags(u32 addr) const noexcept {
        return pages_[addr >> kPageShift].load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::optional<u32> GuestAddressOf(const void* host) const noexcept;
    [[nodiscard]] static std::optional<Area> AreaOf(u32 addr) noexcept;

    // ---- host access through the shadow view; callers validate with Check() ----
    template <typename T>
    [[nodiscard]] T Read(u32 ea) const noexcept {
        static_assert(std::is_integral_v<T>);
        T value{};
        std::memcpy(&value, shadow_ + ea, sizeof(T));
        return ByteSwap(value);
    }
    template <typename T>
    void Write(u32 ea, T value) noexcept {
        static_assert(std::is_integral_v<T>);
        value = ByteSwap(value);
        std::memcpy(shadow_ + ea, &value, sizeof(T));
    }
    // Raw copy of a guest structure declared with be_t<> fields.
    template <typename T>
    [[nodiscard]] T ReadStruct(u32 ea) const noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        T value;
        std::memcpy(&value, shadow_ + ea, sizeof(T));
        return value;
    }
    void CopyIn(u32 ea, const void* src, std::size_t size) noexcept {
        std::memcpy(shadow_ + ea, src, size);
    }
    void Fill(u32 ea, u8 value, std::size_t size) noexcept { std::memset(shadow_ + ea, value, size); }
    // NUL-terminated guest string, page-checked; nullopt if unreadable or longer than max.
    [[nodiscard]] std::optional<std::string> ReadCString(u32 ea, std::size_t max) const;

    // ---- lv2 user-memory accounting (sys_memory_get_user_memory_size) ----
    [[nodiscard]] u64 user_budget() const noexcept { return user_budget_; }
    [[nodiscard]] u64 user_used() const noexcept { return user_used_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool Charge(u64 bytes) noexcept;
    void Uncharge(u64 bytes) noexcept;

private:
    bool HostCommit(u32 addr, u64 size, u8 prot) noexcept;
    void HostRelease(u32 addr, u64 size) noexcept;
    bool HostProtect(u32 addr, u64 size, u8 prot) noexcept;
    void SetFlags(u32 addr, u64 size, u8 flags, bool keep_code) noexcept;
    void NotifyIfCode(u32 addr, u64 size) noexcept;
    void Shutdown() noexcept;

    u8* base_ = nullptr;    // guest view: protections mirror the guest's
    u8* shadow_ = nullptr;  // host view: always RW where committed
#if defined(_WIN32)
    void* section_ = nullptr;  // HANDLE of the pagefile-backed section
    void* guard_ = nullptr;    // trailing placeholder kept inaccessible
#else
    int fd_ = -1;              // memfd backing the arena (file offset == EA)
    u8* reservation_ = nullptr;
#endif
    std::unique_ptr<std::atomic<u8>[]> pages_;
    std::array<GranuleAllocator, kAreaCount> allocators_{};
    std::mutex mutex_;  // allocation/protection changes only; queries are lock-free
    u64 user_budget_ = kDefaultUserMemory;
    std::atomic<u64> user_used_{0};
    InvalidateHook invalidate_hook_ = nullptr;
    void* invalidate_user_ = nullptr;
};

// Fastmem base published for generated code (set by Initialize, cleared on shutdown).
inline u8* g_fastmem_base = nullptr;

}  // namespace cell::vm
