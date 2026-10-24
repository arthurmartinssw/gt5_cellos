// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — common/types.h
//
// Fixed-width types, big-endian wrappers and lv2 (CellOS) error codes.
//
// PS4 -> PS3 note: the bloodborne_pc runtime never byte-swaps anything because Orbis is
// little-endian x86-64 and the guest code runs natively. The Cell PPU is big-endian, so
// every value the loader/HLE reads from or writes to guest memory must be swapped. The
// JIT will do the same with MOVBE/BSWAP on its fastmem loads and stores.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(_MSC_VER) && !defined(__clang__)
#include <stdlib.h>  // _byteswap_*
#endif

namespace cell {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;

// ---------------------------------------------------------------------------------------
// Byte swapping. std::byteswap is C++23; this is the C++20 equivalent that compiles to a
// single BSWAP / ROL instruction on every compiler we target.
// ---------------------------------------------------------------------------------------
template <typename T>
[[nodiscard]] inline T ByteSwap(T value) noexcept {
    static_assert(std::is_integral_v<T>, "ByteSwap needs an integral type");
    if constexpr (sizeof(T) == 1) {
        return value;
    } else if constexpr (sizeof(T) == 2) {
#if defined(_MSC_VER) && !defined(__clang__)
        return static_cast<T>(_byteswap_ushort(static_cast<u16>(value)));
#else
        return static_cast<T>(__builtin_bswap16(static_cast<u16>(value)));
#endif
    } else if constexpr (sizeof(T) == 4) {
#if defined(_MSC_VER) && !defined(__clang__)
        return static_cast<T>(_byteswap_ulong(static_cast<u32>(value)));
#else
        return static_cast<T>(__builtin_bswap32(static_cast<u32>(value)));
#endif
    } else {
        static_assert(sizeof(T) == 8, "unsupported integer width");
#if defined(_MSC_VER) && !defined(__clang__)
        return static_cast<T>(_byteswap_uint64(static_cast<u64>(value)));
#else
        return static_cast<T>(__builtin_bswap64(static_cast<u64>(value)));
#endif
    }
}

namespace detail {
template <std::size_t N> struct UintOfSize;
template <> struct UintOfSize<1> { using type = u8; };
template <> struct UintOfSize<2> { using type = u16; };
template <> struct UintOfSize<4> { using type = u32; };
template <> struct UintOfSize<8> { using type = u64; };
}  // namespace detail

// ---------------------------------------------------------------------------------------
// be_t<T>: a value stored big-endian, exactly as the PPU sees it. Used to describe guest
// structures (sys_process_param_t, libstub tables, sys_memory_info_t...) byte-for-byte so
// a memcpy from guest memory gives the right layout and the getters do the swap.
// ---------------------------------------------------------------------------------------
template <typename T>
struct be_t {
    static_assert(std::is_trivially_copyable_v<T>, "be_t needs a trivially copyable type");
    using Storage = typename detail::UintOfSize<sizeof(T)>::type;

    Storage raw;  // big-endian bytes; never read directly

    [[nodiscard]] T get() const noexcept { return std::bit_cast<T>(ByteSwap(raw)); }
    void set(T value) noexcept { raw = ByteSwap(std::bit_cast<Storage>(value)); }
    operator T() const noexcept { return get(); }  // NOLINT(google-explicit-constructor)
    be_t& operator=(T value) noexcept {
        set(value);
        return *this;
    }
};
static_assert(sizeof(be_t<u32>) == 4 && alignof(be_t<u32>) == 4);
static_assert(sizeof(be_t<u64>) == 8);

// ---------------------------------------------------------------------------------------
// lv2 error codes (CELL_E*). The PS4 runtime returns SCE_KERNEL_ERROR_* (0x8002xxxx, e.g.
// runtime_memory.c: INVALID 0x80020016, NO_MEMORY 0x8002000c). lv2 uses the 0x8001xxxx
// family. Syscalls return them as s32 in r3, sign-extended to 64 bits.
// ---------------------------------------------------------------------------------------
inline constexpr s32 CELL_OK = 0;
inline constexpr s32 CELL_EAGAIN = static_cast<s32>(0x80010001u);
inline constexpr s32 CELL_EINVAL = static_cast<s32>(0x80010002u);
inline constexpr s32 CELL_ENOSYS = static_cast<s32>(0x80010003u);
inline constexpr s32 CELL_ENOMEM = static_cast<s32>(0x80010004u);
inline constexpr s32 CELL_ESRCH = static_cast<s32>(0x80010005u);
inline constexpr s32 CELL_ENOENT = static_cast<s32>(0x80010006u);
inline constexpr s32 CELL_ENOEXEC = static_cast<s32>(0x80010007u);
inline constexpr s32 CELL_EDEADLK = static_cast<s32>(0x80010008u);
inline constexpr s32 CELL_EPERM = static_cast<s32>(0x80010009u);
inline constexpr s32 CELL_EBUSY = static_cast<s32>(0x8001000Au);
inline constexpr s32 CELL_EFAULT = static_cast<s32>(0x8001000Du);
inline constexpr s32 CELL_ETIMEDOUT = static_cast<s32>(0x8001000Bu);
inline constexpr s32 CELL_ESTAT = static_cast<s32>(0x8001000Fu);  // object in the wrong state
inline constexpr s32 CELL_EALIGN = static_cast<s32>(0x80010010u);
inline constexpr s32 CELL_EKRESOURCE = static_cast<s32>(0x80010011u);
inline constexpr s32 CELL_ECANCELED = static_cast<s32>(0x80010013u);
inline constexpr s32 CELL_EEXIST = static_cast<s32>(0x80010014u);
inline constexpr s32 CELL_EISCONN = static_cast<s32>(0x80010015u);
inline constexpr s32 CELL_ENOTCONN = static_cast<s32>(0x80010016u);
// File-system family (cellFs / sys_fs return these too).
inline constexpr s32 CELL_EISDIR = static_cast<s32>(0x80010012u);
inline constexpr s32 CELL_EFBIG = static_cast<s32>(0x80010020u);
inline constexpr s32 CELL_ENOSPC = static_cast<s32>(0x80010023u);
inline constexpr s32 CELL_EROFS = static_cast<s32>(0x80010026u);
inline constexpr s32 CELL_EACCES = static_cast<s32>(0x80010029u);
inline constexpr s32 CELL_EBADF = static_cast<s32>(0x8001002Au);
inline constexpr s32 CELL_EIO = static_cast<s32>(0x8001002Bu);
inline constexpr s32 CELL_EMFILE = static_cast<s32>(0x8001002Cu);
inline constexpr s32 CELL_ENOTDIR = static_cast<s32>(0x8001002Eu);
inline constexpr s32 CELL_EXDEV = static_cast<s32>(0x80010030u);
inline constexpr s32 CELL_ENAMETOOLONG = static_cast<s32>(0x80010034u);
inline constexpr s32 CELL_ENOTEMPTY = static_cast<s32>(0x80010036u);
inline constexpr s32 CELL_ENOTSUP = static_cast<s32>(0x80010037u);
inline constexpr s32 CELL_ENOTMOUNTED = static_cast<s32>(0x8001003Au);

// Alignment helpers (power-of-two alignments only).
template <typename T>
[[nodiscard]] constexpr T AlignDown(T value, T align) noexcept {
    return value & ~(align - 1);
}
template <typename T>
[[nodiscard]] constexpr T AlignUp(T value, T align) noexcept {
    return (value + align - 1) & ~(align - 1);
}
template <typename T>
[[nodiscard]] constexpr bool IsPow2(T value) noexcept {
    return value && !(value & (value - 1));
}

}  // namespace cell
