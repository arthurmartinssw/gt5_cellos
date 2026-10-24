// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — loader/param_sfo.cpp
#include "loader/param_sfo.h"

namespace cell::loader {

namespace {
u32 Le32(const std::vector<u8>& d, std::size_t at) {
    return u32{d[at]} | (u32{d[at + 1]} << 8) | (u32{d[at + 2]} << 16) | (u32{d[at + 3]} << 24);
}
u16 Le16(const std::vector<u8>& d, std::size_t at) { return static_cast<u16>(d[at] | (d[at + 1] << 8)); }
}  // namespace

std::optional<ParamSfo> ParseParamSfo(const std::vector<u8>& d) {
    if (d.size() < 20 || d[0] != 0 || d[1] != 'P' || d[2] != 'S' || d[3] != 'F') return std::nullopt;
    const u64 key_table = Le32(d, 8), data_table = Le32(d, 12), count = Le32(d, 16);
    if (20 + count * 16 > d.size() || key_table > d.size() || data_table > d.size()) return std::nullopt;
    ParamSfo out;
    for (u64 i = 0; i < count; ++i) {
        const std::size_t e = static_cast<std::size_t>(20 + i * 16);
        const u64 key_at = key_table + Le16(d, e);
        const u16 fmt = Le16(d, e + 2);
        const u64 len = Le32(d, e + 4), data_at = data_table + Le32(d, e + 12);
        if (key_at >= d.size() || data_at + len > d.size()) return std::nullopt;
        std::string key;
        for (std::size_t k = static_cast<std::size_t>(key_at); k < d.size() && d[k]; ++k) key += static_cast<char>(d[k]);
        SfoValue v;
        v.fmt = fmt;
        if (fmt == 0x0404) {
            if (len < 4) return std::nullopt;
            v.integer = Le32(d, static_cast<std::size_t>(data_at));
        } else {
            v.text.assign(reinterpret_cast<const char*>(d.data() + data_at), static_cast<std::size_t>(len));
            while (!v.text.empty() && v.text.back() == '\0') v.text.pop_back();
        }
        out[key] = std::move(v);
    }
    return out;
}

}  // namespace cell::loader
