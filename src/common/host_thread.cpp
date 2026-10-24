// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — common/host_thread.cpp
#include "common/host_thread.h"

#include <chrono>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002  // Windows 10 1803+; missing in older SDK/MinGW headers
#endif
#elif defined(__linux__)
#include <pthread.h>
#endif

namespace cell::host {

void SetCurrentThreadName(const char* name) noexcept {
    if (!name) return;
#if defined(_WIN32)
    // SetThreadDescription (Windows 10 1607+) is resolved at run time, like the placeholder
    // APIs in guest_memory.cpp, so neither the SDK version nor MinGW's import libs matter.
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const auto set_description = reinterpret_cast<SetThreadDescriptionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (!set_description) return;
    wchar_t wide[64];
    int i = 0;
    for (; name[i] && i < 63; ++i) wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(name[i]));
    wide[i] = L'\0';
    set_description(GetCurrentThread(), wide);
#elif defined(__linux__)
    // The kernel keeps 15 characters + NUL.
    char shortened[16];
    std::strncpy(shortened, name, sizeof(shortened) - 1);
    shortened[sizeof(shortened) - 1] = '\0';
    pthread_setname_np(pthread_self(), shortened);
#else
    (void)name;  // other hosts: not needed for correctness
#endif
}

void SleepMicroseconds(std::uint32_t microseconds) noexcept {
    if (microseconds == 0) {
        std::this_thread::yield();
        return;
    }
#if defined(_WIN32)
    // One timer per thread, created on first use and closed when the thread exits.
    struct Timer {
        HANDLE handle = nullptr;
        bool tried = false;
        ~Timer() {
            if (handle) CloseHandle(handle);
        }
    };
    thread_local Timer timer;
    if (!timer.tried) {
        timer.tried = true;
        timer.handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    if (timer.handle) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(microseconds) * 10;  // relative, 100 ns units
        if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer.handle, INFINITE);
            return;
        }
    }
    Sleep((microseconds + 999) / 1000);  // pre-1803: whole milliseconds, rounded up
#else
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
#endif
}

}  // namespace cell::host
