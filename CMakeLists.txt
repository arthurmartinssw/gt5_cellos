# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(gt5_cellos LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_library(cellos_runtime STATIC
  src/memory/guest_memory.cpp
  src/lv2/sys_memory.cpp
  src/hle/hle_registry.cpp
  src/loader/self.cpp
  src/loader/ppu_loader.cpp
  src/ppu/ppu_decoder.cpp
  src/ppu/ppu_interpreter.cpp
  src/hle/nid_names.cpp
  src/hle/guest_format.cpp
  src/hle/sys_prx_for_user.cpp
  src/lv2/sys_process.cpp
  src/ppu/ppu_code_cache.cpp
  src/lv2/kernel.cpp
  src/lv2/sys_ppu_thread.cpp
  src/lv2/sys_sync.cpp
  src/lv2/sys_event.cpp
  src/hle/cell_sysmodule.cpp
  src/lv2/sys_spu.cpp
  src/common/host_thread.cpp
  src/spu/spu_elf.cpp
  src/spu/spu_thread.cpp
  src/spu/spu_mfc.cpp
  src/spu/spu_decoder.cpp
  src/spu/spu_interpreter.cpp
  src/spu/spu_run_stats.cpp
  src/hle/sys_spu_hle.cpp
  src/hle/cell_libprof.cpp
  src/hle/cell_fs.cpp
  src/hle/cell_sysutil.cpp
  src/hle/cell_game.cpp
  src/hle/net_offline.cpp
  src/hle/cell_audio.cpp
  src/hle/cell_usbd.cpp
  src/hle/cell_pad.cpp
  src/hle/cell_spurs.cpp
  src/lv2/sys_mmapper.cpp
  src/rsx/rsx_fifo.cpp
  src/hle/gcm_interrupts.cpp
  src/hle/cell_gcm_sys.cpp
  src/loader/ppu_linker.cpp
  src/loader/prx_loader.cpp
  src/loader/param_sfo.cpp
  src/lv2/vfs.cpp
  src/lv2/stall_monitor.cpp
  src/lv2/sys_fs.cpp
  src/lv2/sys_timer.cpp
  src/common/host_file.cpp
  src/lv2/sys_prx.cpp
  src/lv2/syscall_names.cpp
  src/ppu/ppu_disasm.cpp
)
target_include_directories(cellos_runtime PUBLIC src)
target_link_libraries(cellos_runtime PUBLIC ZLIB::ZLIB Threads::Threads)

if(WIN32)
  # Placeholder APIs (VirtualAlloc2 / MapViewOfFile3) are resolved at run time: Windows 10 1803+.
  target_compile_definitions(cellos_runtime PUBLIC _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(MSVC)
  target_compile_definitions(cellos_runtime PUBLIC _CRT_SECURE_NO_WARNINGS)
  target_compile_options(cellos_runtime PUBLIC /W4 /permissive- /Zc:__cplusplus /utf-8)
else()
  target_compile_options(cellos_runtime PUBLIC -Wall -Wextra -Wpedantic)
endif()

add_executable(gt5_cellos src/main.cpp)
target_link_libraries(gt5_cellos PRIVATE cellos_runtime)

enable_testing()
add_executable(loader_tests tests/loader_tests.cpp)
target_link_libraries(loader_tests PRIVATE cellos_runtime)
add_test(NAME loader_tests COMMAND loader_tests)
add_executable(ppu_tests tests/ppu_tests.cpp)
target_link_libraries(ppu_tests PRIVATE cellos_runtime)
add_test(NAME ppu_tests COMMAND ppu_tests)
add_executable(kernel_tests tests/kernel_tests.cpp)
target_link_libraries(kernel_tests PRIVATE cellos_runtime)
add_test(NAME kernel_tests COMMAND kernel_tests)

add_executable(spu_tests tests/spu_tests.cpp)
target_link_libraries(spu_tests PRIVATE cellos_runtime)
add_test(NAME spu_tests COMMAND spu_tests)

add_executable(gcm_tests tests/gcm_tests.cpp)
target_link_libraries(gcm_tests PRIVATE cellos_runtime)
add_test(NAME gcm_tests COMMAND gcm_tests)

add_executable(spu_isa_tests tests/spu_isa_tests.cpp)
target_link_libraries(spu_isa_tests PRIVATE cellos_runtime)
add_test(NAME spu_isa_tests COMMAND spu_isa_tests)

add_executable(prx_tests tests/prx_tests.cpp)
target_link_libraries(prx_tests PRIVATE cellos_runtime)
add_test(NAME prx_tests COMMAND prx_tests)

add_executable(libsre_support_tests tests/libsre_support_tests.cpp)
target_link_libraries(libsre_support_tests PRIVATE cellos_runtime)
add_test(NAME libsre_support_tests COMMAND libsre_support_tests)

add_executable(fs_tests tests/fs_tests.cpp)
target_link_libraries(fs_tests PRIVATE cellos_runtime)
add_test(NAME fs_tests COMMAND fs_tests)

add_executable(paging_tests tests/paging_tests.cpp)
target_link_libraries(paging_tests PRIVATE cellos_runtime)
add_test(NAME paging_tests COMMAND paging_tests)

add_executable(boot_hle_tests tests/boot_hle_tests.cpp)
target_link_libraries(boot_hle_tests PRIVATE cellos_runtime)
add_test(NAME boot_hle_tests COMMAND boot_hle_tests)

add_executable(audio_tests tests/audio_tests.cpp)
target_link_libraries(audio_tests PRIVATE cellos_runtime)
add_test(NAME audio_tests COMMAND audio_tests)

add_executable(spu_lifecycle_tests tests/spu_lifecycle_tests.cpp)
target_link_libraries(spu_lifecycle_tests PRIVATE cellos_runtime)
add_test(NAME spu_lifecycle_tests COMMAND spu_lifecycle_tests)

add_executable(timer_pad_sync_tests tests/timer_pad_sync_tests.cpp)
target_link_libraries(timer_pad_sync_tests PRIVATE cellos_runtime)
add_test(NAME timer_pad_sync_tests COMMAND timer_pad_sync_tests)

add_executable(hle_output_tests tests/hle_output_tests.cpp)
target_link_libraries(hle_output_tests PRIVATE cellos_runtime)
add_test(NAME hle_output_tests COMMAND hle_output_tests)
