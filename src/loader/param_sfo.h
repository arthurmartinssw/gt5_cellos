// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — loader/param_sfo.h
//
// PARAM.SFO reader. The SFO is the game's metadata table (PS3_GAME/PARAM.SFO next to
// USRDIR/EBOOT.BIN). lv2 keeps a summary of it per process — sys_process_get_paramsfo hands
// the title id to libraries such as libsre, which tag their SPU groups and traces with it.
//
// Layout (little endian, unlike everything else on the PS3):
//   header (20 bytes): magic "\0PSF", version, key_table_start, data_table_start, entry_count
//   index  (16 bytes each): u16 key_offset, u16 data_fmt, u32 data_len, u32 data_max_len,
//                           u32 data_offset
//   data_fmt: 0x0004 utf8-special (not NUL terminated), 0x0204 utf8, 0x0404 int32
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common/types.h"

namespace cell::loader {

struct SfoValue {
    u16 fmt = 0;
    std::string text;  // utf8 entries (trailing NULs stripped)
    u32 integer = 0;   // int32 entries
};

using ParamSfo = std::map<std::string, SfoValue>;

// Returns nullopt when the data is not a well-formed SFO (bad magic, entries out of bounds).
[[nodiscard]] std::optional<ParamSfo> ParseParamSfo(const std::vector<u8>& data);

}  // namespace cell::loader
