// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/fnid.h
//
// PS3 function NIDs (FNIDs): the first 4 bytes of SHA-1(name || suffix), read little-endian.
// The PS4 equivalent in bloodborne_pc is scripts/prepare.py nid(): SHA-1 with a different
// salt, 8 bytes, base64-encoded. Verified against 16 known sysPrxForUser FNIDs
// (sys_initialize_tls = 0x744680A2, sys_process_exit = 0xE6F2C1E7, ...): see tests.
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include "common/types.h"

namespace cell::hle {

// Small SHA-1 (FIPS 180-1). Only used at start-up to build name tables.
class Sha1 {
public:
    void Update(const void* data, std::size_t size) noexcept {
        const auto* p = static_cast<const u8*>(data);
        length_ += size;
        while (size) {
            const std::size_t take = std::min(size, std::size_t{64} - fill_);
            std::memcpy(block_ + fill_, p, take);
            fill_ += take;
            p += take;
            size -= take;
            if (fill_ == 64) {
                Compress();
                fill_ = 0;
            }
        }
    }
    std::array<u8, 20> Final() noexcept {
        const u64 bits = length_ * 8;
        const u8 pad = 0x80;
        Update(&pad, 1);
        const u8 zero = 0;
        while (fill_ != 56) Update(&zero, 1);
        u8 len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<u8>(bits >> (56 - 8 * i));
        Update(len, 8);
        std::array<u8, 20> digest{};
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 4; ++j) digest[4 * i + j] = static_cast<u8>(h_[i] >> (24 - 8 * j));
        return digest;
    }

private:
    static u32 Rol(u32 x, int n) noexcept { return (x << n) | (x >> (32 - n)); }
    void Compress() noexcept {
        u32 w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (u32{block_[4 * i]} << 24) | (u32{block_[4 * i + 1]} << 16) | (u32{block_[4 * i + 2]} << 8) |
                   block_[4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            u32 f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const u32 temp = Rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rol(b, 30);
            b = a;
            a = temp;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    u32 h_[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    u8 block_[64] = {};
    std::size_t fill_ = 0;
    u64 length_ = 0;
};

[[nodiscard]] inline u32 ComputeFnid(std::string_view name) noexcept {
    static constexpr u8 kSuffix[16] = {0x67, 0x59, 0x65, 0x99, 0x04, 0x25, 0x04, 0x90,
                                       0x56, 0x64, 0x27, 0x49, 0x94, 0x89, 0x74, 0x1A};
    Sha1 sha;
    sha.Update(name.data(), name.size());
    sha.Update(kSuffix, sizeof(kSuffix));
    const auto digest = sha.Final();
    return u32{digest[0]} | (u32{digest[1]} << 8) | (u32{digest[2]} << 16) | (u32{digest[3]} << 24);
}

// Known export name for an FNID, or nullptr. Built once from nid_names.cpp.
[[nodiscard]] const char* LookupFunctionName(u32 fnid);

}  // namespace cell::hle
