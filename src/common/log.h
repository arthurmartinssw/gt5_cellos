// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — common/log.h
//
// Minimal printf-style logging. Kept header-only and allocation-free so it is safe to call
// from HLE handlers on guest threads. A real build swaps this for an async ring-buffer
// logger (the PS4 runtime prints straight to stdout/stderr, same as this).
#pragma once

#include <cstdarg>
#include <cstdio>

namespace cell::log {

enum class Level { Trace, Info, Warn, Error };

inline Level& Threshold() noexcept {
    static Level level = Level::Info;
    return level;
}

// Name of the guest thread running on this host thread ("main", "0x01000001 GameMain"...),
// printed in every line so interleaved multi-thread logs stay readable.
inline thread_local const char* tls_thread_name = nullptr;

#if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)  // MinGW's printf checker is the MS one
__attribute__((format(printf, 3, 4)))
#endif
inline void Write(Level level, const char* channel, const char* format, ...) noexcept {
    if (level < Threshold()) return;
    static constexpr const char* kNames[] = {"T", "I", "W", "E"};
    std::FILE* out = level >= Level::Warn ? stderr : stdout;
    if (tls_thread_name) std::fprintf(out, "[%s][%s][%s] ", kNames[static_cast<int>(level)], tls_thread_name, channel);
    else std::fprintf(out, "[%s][%s] ", kNames[static_cast<int>(level)], channel);
    va_list args;
    va_start(args, format);
    std::vfprintf(out, format, args);
    va_end(args);
    std::fputc('\n', out);
}

}  // namespace cell::log

#define LOG_TRACE(ch, ...) ::cell::log::Write(::cell::log::Level::Trace, ch, __VA_ARGS__)
#define LOG_INFO(ch, ...) ::cell::log::Write(::cell::log::Level::Info, ch, __VA_ARGS__)
#define LOG_WARN(ch, ...) ::cell::log::Write(::cell::log::Level::Warn, ch, __VA_ARGS__)
#define LOG_ERROR(ch, ...) ::cell::log::Write(::cell::log::Level::Error, ch, __VA_ARGS__)
