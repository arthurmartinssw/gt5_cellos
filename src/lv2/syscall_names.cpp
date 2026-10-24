// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/syscall_names.cpp
//
// Names of lv2 syscalls not implemented yet, so an "unimplemented syscall N" warning says
// what the game wanted. Numbers follow RPCS3's lv2 table (Emu/Cell/lv2/lv2.cpp). Only ranges
// whose numbering is well established are listed; a wrong entry here only mislabels a log line.
// Implemented syscalls carry their own name in SyscallTable.
#include "lv2/syscall_table.h"

namespace cell::lv2 {

const char* KnownSyscallName(u32 number) noexcept {
    switch (number) {
    // SPU threads and thread groups (phase 4)
    // page faults (demand paging, phase 7.3)
    case 58: return "sys_ppu_thread_get_page_fault_context";
    // trace (debugger) — verified against RPCS3 lv2.cpp
    case 60: return "sys_trace_create";
    case 61: return "sys_trace_start";
    case 62: return "sys_trace_stop";
    case 63: return "sys_trace_update_top_index";
    case 64: return "sys_trace_destroy";
    case 65: return "sys_trace_drain";
    case 66: return "sys_trace_attach_process";
    case 67: return "sys_trace_allocate_buffer";
    case 68: return "sys_trace_free_buffer";
    case 69: return "sys_trace_create2";
    case 199: return "sys_raw_spu_recover_page_fault";
    case 160: return "sys_raw_spu_create";
    case 161: return "sys_raw_spu_destroy";
    case 163: return "sys_raw_spu_read_puint_mb";
    case 165: return "sys_spu_thread_get_exit_status";
    case 166: return "sys_spu_thread_set_argument";
    case 167: return "sys_spu_thread_group_start_on_exit";
    case 169: return "sys_spu_initialize";
    case 170: return "sys_spu_thread_group_create";
    case 171: return "sys_spu_thread_group_destroy";
    case 172: return "sys_spu_thread_initialize";
    case 173: return "sys_spu_thread_group_start";
    case 174: return "sys_spu_thread_group_suspend";
    case 175: return "sys_spu_thread_group_resume";
    case 176: return "sys_spu_thread_group_yield";
    case 177: return "sys_spu_thread_group_terminate";
    case 178: return "sys_spu_thread_group_join";
    case 179: return "sys_spu_thread_group_set_priority";
    case 180: return "sys_spu_thread_group_get_priority";
    case 181: return "sys_spu_thread_write_ls";
    case 182: return "sys_spu_thread_read_ls";
    case 184: return "sys_spu_thread_write_snr";
    case 185: return "sys_spu_thread_group_connect_event";
    case 186: return "sys_spu_thread_group_disconnect_event";
    case 187: return "sys_spu_thread_set_spu_cfg";
    case 188: return "sys_spu_thread_get_spu_cfg";
    case 190: return "sys_spu_thread_write_spu_mb";
    case 191: return "sys_spu_thread_connect_event";
    case 192: return "sys_spu_thread_disconnect_event";
    case 193: return "sys_spu_thread_bind_queue";
    case 194: return "sys_spu_thread_unbind_queue";
    // memory mapper (326, 329-332, 334, 335, 337 are implemented)
    case 327: return "sys_mmapper_enable_page_fault_notification";
    case 333: return "sys_mmapper_set_shared_memory_flag";
    case 336: return "sys_mmapper_change_address_access_right";
    case 338: return "sys_mmapper_get_shared_memory_attribute";
    case 339: return "sys_mmapper_allocate_shared_memory_ext";
    case 341: return "sys_memory_container_create";
    case 342: return "sys_memory_container_destroy";
    case 343: return "sys_memory_container_get_size";
    case 350: return "sys_memory_allocate_from_container";
    case 351: return "sys_memory_get_page_attribute";
    case 353: return "sys_memory_get_user_memory_stat";
    // RSX driver (libgcm talks to these on hardware; our cellGcmSys is HLE and skips them)
    case 666: return "sys_rsx_device_open";
    case 667: return "sys_rsx_device_close";
    case 668: return "sys_rsx_memory_allocate";
    case 669: return "sys_rsx_memory_free";
    case 670: return "sys_rsx_context_allocate";
    case 671: return "sys_rsx_context_free";
    case 672: return "sys_rsx_context_iomap";
    case 673: return "sys_rsx_context_iounmap";
    case 674: return "sys_rsx_context_attribute";
    case 675: return "sys_rsx_device_map";
    case 676: return "sys_rsx_device_unmap";
    case 677: return "sys_rsx_attribute";
    // PRX (480-483 and 496 are implemented)
    case 484: return "_sys_prx_register_module";
    case 485: return "_sys_prx_query_module";
    case 486: return "_sys_prx_register_library";
    case 487: return "_sys_prx_unregister_library";
    case 488: return "_sys_prx_link_library";
    case 489: return "_sys_prx_unlink_library";
    case 490: return "_sys_prx_query_library";
    case 494: return "_sys_prx_get_module_list";
    case 495: return "_sys_prx_get_module_info";
    case 497: return "_sys_prx_load_module_on_memcontainer";
    // SPU thread group extras (250-252 are implemented)
    case 253: return "sys_spu_thread_group_syscall_253";
    case 254: return "sys_spu_thread_group_log";
    // file system (cellFs goes through these)
    case 801: return "sys_fs_open";
    case 802: return "sys_fs_read";
    case 803: return "sys_fs_write";
    case 804: return "sys_fs_close";
    case 805: return "sys_fs_opendir";
    case 806: return "sys_fs_readdir";
    case 807: return "sys_fs_closedir";
    case 808: return "sys_fs_stat";
    case 809: return "sys_fs_fstat";
    case 818: return "sys_fs_lseek";
    default: return nullptr;
    }
}

}  // namespace cell::lv2
