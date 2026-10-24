// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/guest_format.cpp
#include "hle/guest_format.h"

#include <bit>
#include <cstdio>

namespace cell::hle {

namespace {

// Variadic arguments either from a call (registers, then the caller's save area) or from a
// va_list (consecutive 8-byte slots in memory).
class ArgReader {
public:
    ArgReader(const vm::GuestMemory& memory, const ppu::PpuThreadContext& ctx, u32 index)
        : memory_(memory), ctx_(&ctx), index_(index) {}
    ArgReader(const vm::GuestMemory& memory, u32 va_list_ea) : memory_(memory), va_(va_list_ea) {}
    u64 Next() {
        const u32 i = index_++;
        u32 slot;
        if (ctx_) {
            if (i < 8) return ctx_->gpr[3 + i];
            slot = static_cast<u32>(ctx_->gpr[1]) + 48 + 8 * i;
        } else {
            slot = va_ + 8 * i;
        }
        return memory_.Check(slot, 8, vm::kPageReadable) ? memory_.Read<u64>(slot) : 0;
    }

private:
    const vm::GuestMemory& memory_;
    const ppu::PpuThreadContext* ctx_ = nullptr;
    u32 va_ = 0;
    u32 index_ = 0;
};

std::string Format(const vm::GuestMemory& memory, u32 format_ea, ArgReader& args);

}  // namespace

std::string FormatGuest(const vm::GuestMemory& memory, const ppu::PpuThreadContext& ctx, u32 format_ea,
                        u32 first_vararg) {
    ArgReader args(memory, ctx, first_vararg);
    return Format(memory, format_ea, args);
}

std::string FormatGuestVa(const vm::GuestMemory& memory, u32 format_ea, u32 va_list_ea) {
    ArgReader args(memory, va_list_ea);
    return Format(memory, format_ea, args);
}

namespace {
std::string Format(const vm::GuestMemory& memory, u32 format_ea, ArgReader& args) {
    const auto format = memory.ReadCString(format_ea, 4096);
    if (!format) return "<unreadable format string>";
    std::string out;
    const std::string& f = *format;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') {
            out.push_back(f[i]);
            continue;
        }
        // %[flags][width][.precision][length]conversion -> rebuilt as a host spec.
        std::string spec = "%";
        std::size_t j = i + 1;
        while (j < f.size() && std::string_view("-+ #0").find(f[j]) != std::string_view::npos) spec.push_back(f[j++]);
        auto take_number = [&] {
            if (j < f.size() && f[j] == '*') {
                spec += std::to_string(static_cast<s32>(args.Next()));
                ++j;
            }
            while (j < f.size() && f[j] >= '0' && f[j] <= '9') spec.push_back(f[j++]);
        };
        take_number();
        if (j < f.size() && f[j] == '.') {
            spec.push_back(f[j++]);
            take_number();
        }
        int longs = 0;
        while (j < f.size() && std::string_view("hlLqjzt").find(f[j]) != std::string_view::npos) {
            if (f[j] == 'l' || f[j] == 'L' || f[j] == 'q' || f[j] == 'j' || f[j] == 'z' || f[j] == 't') ++longs;
            ++j;
        }
        if (j >= f.size()) break;
        const char conv = f[j];
        i = j;
        char buffer[512];
        switch (conv) {
        case 'd':
        case 'i': {
            const u64 v = args.Next();
            const long long value = longs ? static_cast<long long>(v) : static_cast<long long>(static_cast<s32>(v));
            std::snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(), value);
            out += buffer;
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            const u64 v = args.Next();
            const unsigned long long value = longs ? v : static_cast<u32>(v);
            std::snprintf(buffer, sizeof(buffer), (spec + "ll" + conv).c_str(), value);
            out += buffer;
            break;
        }
        case 'p':
            std::snprintf(buffer, sizeof(buffer), "0x%08x", static_cast<u32>(args.Next()));
            out += buffer;
            break;
        case 'c':
            out.push_back(static_cast<char>(args.Next()));
            break;
        case 's': {
            const auto s = memory.ReadCString(static_cast<u32>(args.Next()), 4096);
            std::snprintf(buffer, sizeof(buffer), (spec + "s").c_str(), s ? s->c_str() : "(bad string)");
            out += buffer;
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            // Variadic doubles are passed in GPRs/stack slots as raw bits under the PPC64 ABI.
            std::snprintf(buffer, sizeof(buffer), (spec + conv).c_str(), std::bit_cast<f64>(args.Next()));
            out += buffer;
            break;
        case '%':
            out.push_back('%');
            break;
        case 'n':
            args.Next();
            break;
        default:
            out += spec;
            out.push_back(conv);
        }
    }
    return out;
}
}  // namespace

}  // namespace cell::hle
