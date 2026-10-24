// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — hle/nid_names.cpp
//
// Export names of the PS3 system libraries a game typically imports. FNIDs are *computed*
// from these names (ComputeFnid), never typed by hand, so a typo here only means "no name
// shown", never a wrong binding. Extend freely: unknown imports are reported with their NID
// and module, which is how new names get added.
#include <unordered_map>

#include "hle/fnid.h"

namespace cell::hle {

namespace {

constexpr const char* kNames[] = {
    // ---- sysPrxForUser (liblv2 user-side helpers) ----
    "sys_initialize_tls", "sys_process_exit", "sys_process_is_stack", "sys_process_get_paramsfo",
    "sys_game_process_exitspawn", "sys_game_process_exitspawn2", "_sys_process_atexitspawn",
    "_sys_process_at_Exitspawn", "sys_prx_exitspawn_with_level", "sys_time_get_system_time",
    "sys_ppu_thread_get_id", "sys_ppu_thread_exit", "sys_ppu_thread_create", "sys_ppu_thread_once",
    "sys_lwmutex_create", "sys_lwmutex_destroy", "sys_lwmutex_lock", "sys_lwmutex_trylock", "sys_lwmutex_unlock",
    "sys_lwcond_create", "sys_lwcond_destroy", "sys_lwcond_signal", "sys_lwcond_signal_all", "sys_lwcond_wait",
    "sys_prx_load_module", "sys_prx_start_module", "sys_prx_get_module_id_by_name", "sys_prx_stop_module", "sys_prx_unload_module",
    "sys_prx_register_library", "sys_prx_get_module_list", "sys_prx_load_module_list",
    "sys_mmapper_allocate_memory", "sys_mmapper_free_memory", "sys_mmapper_map_memory", "sys_mmapper_unmap_memory",
    "sys_spu_image_import", "sys_spu_image_close", "sys_spu_elf_get_information", "sys_spu_elf_get_segments",
    "sys_raw_spu_load", "sys_raw_spu_image_load", "sys_spu_printf_initialize", "sys_spu_printf_finalize",
    "sys_spu_printf_attach_group", "sys_spu_printf_detach_group", "sys_spu_printf_attach_thread",
    "sys_spu_printf_detach_thread", "sys_get_random_number", "sys_interrupt_thread_disestablish",
    "console_getc", "console_putc", "console_write", "_sys_printf", "_sys_sprintf", "_sys_snprintf",
    "_sys_vprintf", "_sys_vsprintf", "_sys_vsnprintf", "_sys_strlen", "_sys_strcpy", "_sys_strncpy",
    "_sys_strcat", "_sys_strncat", "_sys_strcmp", "_sys_strncmp", "_sys_strchr", "_sys_strrchr",
    "_sys_strncasecmp", "_sys_memset", "_sys_memcpy", "_sys_memmove", "_sys_memcmp", "_sys_memchr",
    "_sys_tolower", "_sys_toupper", "_sys_malloc", "_sys_free", "_sys_memalign", "_sys_heap_create_heap",
    "_sys_heap_delete_heap", "_sys_heap_malloc", "_sys_heap_memalign", "_sys_heap_free",
    "_sys_heap_stats", "_sys_heap_get_total_free_size", "_sys_heap_get_mallinfo", "_sys_qsort",
    // ---- cellSysmodule ----
    "cellSysmoduleInitialize", "cellSysmoduleFinalize", "cellSysmoduleLoadModule", "cellSysmoduleUnloadModule",
    "cellSysmoduleIsLoaded", "cellSysmoduleSetMemcontainer",
    // ---- cellSysutil ----
    "cellSysutilCheckCallback", "cellSysutilRegisterCallback", "cellSysutilUnregisterCallback",
    "cellSysutilGetSystemParamInt", "cellSysutilGetSystemParamString", "cellVideoOutGetState",
    "cellVideoOutGetResolution", "cellVideoOutConfigure", "cellVideoOutGetConfiguration",
    "cellAudioOutGetState", "cellAudioOutConfigure", "cellAudioOutGetSoundAvailability",
    "cellMsgDialogOpen2", "cellMsgDialogClose", "cellSaveDataAutoSave2", "cellSaveDataAutoLoad2",
    // ---- cellGame ----
    "cellGameBootCheck", "cellGameDataCheck", "cellGameContentPermit", "cellGameGetParamInt",
    "cellGameGetParamString", "cellGameGetSizeKB", "cellGamePatchCheck", "cellGameDataCheckCreate2",
    // ---- cellFs / sys_fs ----
    "cellFsOpen", "cellFsClose", "cellFsRead", "cellFsWrite", "cellFsLseek", "cellFsStat", "cellFsFstat",
    "cellFsMkdir", "cellFsRename", "cellFsUnlink", "cellFsOpendir", "cellFsReaddir", "cellFsClosedir",
    "cellFsGetFreeSize", "cellFsFsync", "cellFsTruncate", "cellFsAioInit", "cellFsAioRead", "cellFsAioFinish",
    "cellFsAioWrite", "cellFsAioCancel", "cellFsStReadInit", "cellFsStReadFinish", "cellFsStReadGetRingBuf",
    "cellFsStReadGetStatus", "cellFsStReadGetRegid", "cellFsStReadStart", "cellFsStReadStop", "cellFsStRead",
    "cellFsStReadGetCurrentAddr", "cellFsStReadPutCurrentAddr", "cellFsStReadWait", "cellFsStReadWaitCallback",
    "cellFsSdataOpen", "cellFsSdataOpenByFd", "cellFsSdataOpenWithVersion", "cellFsGetDirectoryEntries",
    "cellFsAllocateFileAreaWithoutZeroFill", "cellFsAllocateFileAreaByFdWithoutZeroFill", "cellFsLink",
    "cellFsChangeFileSizeWithoutAllocation", "cellFsRegisterConversionCallback", "cellFsUnregisterL10nCallbacks",
    "cellFsArcadeHddSerialNumber", "cellFsTruncate2", "cellFsGetBlockSize", "cellFsFGetBlockSize",
    // ---- cellGcmSys ----
    "_cellGcmInitBody", "cellGcmGetConfiguration", "cellGcmMapMainMemory", "cellGcmMapEaIoAddress",
    "cellGcmAddressToOffset", "cellGcmSetFlipMode", "cellGcmGetTiledPitchSize", "cellGcmSetDisplayBuffer",
    "cellGcmGetControlRegister", "cellGcmGetLabelAddress", "cellGcmSetFlipHandler", "cellGcmSetVBlankHandler",
    "cellGcmResetFlipStatus", "cellGcmGetFlipStatus", "cellGcmSetWaitFlip", "cellGcmSetTileInfo",
    "cellGcmBindTile", "cellGcmSetZcull", "cellGcmBindZcull", "cellGcmGetDefaultCommandWordSize",
    "cellGcmGetDefaultSegmentWordSize", "cellGcmInitDefaultFifoMode", "cellGcmSetDefaultFifoSize",
    "cellGcmGetCurrentField", "cellGcmSetFlipCommand", "_cellGcmSetFlipCommand",
    // ---- cellPad / cellKb / cellMouse ----
    "cellPadInit", "cellPadEnd", "cellPadGetData", "cellPadGetInfo", "cellPadGetInfo2", "cellPadClearBuf",
    "cellPadSetPortSetting", "cellPadSetActDirect", "cellPadSetPressMode", "cellPadSetSensorMode",
    // ---- cellAudio ----
    "cellAudioInit", "cellAudioQuit", "cellAudioPortOpen", "cellAudioPortStart", "cellAudioPortStop",
    "cellAudioPortClose", "cellAudioGetPortConfig", "cellAudioSetNotifyEventQueue",
    "cellAudioCreateNotifyEventQueue", "cellAudioGetPortTimestamp",
    // ---- cellSpurs / cellSync (SPU runtime, phase 4) ----
    "cellSpursInitialize", "cellSpursInitializeWithAttribute", "cellSpursInitializeWithAttribute2",
    "cellSpursFinalize", "cellSpursAttachLv2EventQueue", "cellSpursCreateTaskset", "cellSpursCreateTask",
    "cellSpursAttributeInitialize", "cellSpursAttributeSetNamePrefix", "cellSpursAttributeEnableSpuPrintfIfAvailable",
    "cellSpursAttributeSetSpuThreadGroupType", "cellSpursAttributeSetMemoryContainerForSpuThread",
    "cellSpursAttributeEnableSystemWorkload", "cellSpursDetachLv2EventQueue", "cellSpursSetMaxContention",
    "cellSpursSetPriorities", "cellSpursSetPreemptionVictimHints", "cellSpursCreateTasksetWithAttribute",
    "cellSpursCreateTaskWithAttribute", "cellSpursAddWorkload", "cellSpursAddWorkloadWithAttribute",
    "cellSpursCreateJobChain", "cellSpursCreateJobChainWithAttribute", "cellSpursGetSpuThreadGroupId",
    "cellSpursGetNumSpuThread", "cellSpursGetSpuThreadId", "cellSpursEventFlagInitialize",
    "cellSpursEventFlagAttachLv2EventQueue", "cellSpursQueueInitialize", "cellSpursJoinTaskset",
    "cellSpursShutdownTaskset", "cellSyncMutexInitialize",
    "cellSyncMutexLock", "cellSyncMutexUnlock", "cellSyncBarrierInitialize", "cellSyncQueueInitialize",
    // ---- cellResc / misc ----
    "cellRescInit", "cellRescExit", "cellRescSetDisplayMode", "cellRescSetConvertAndFlip",
    "cellNetCtlInit", "cellNetCtlTerm", "cellSysCacheMount", "cellSysCacheClear", "cellRtcGetCurrentTick",
    "cellRtcGetCurrentClockLocalTime", "cellUserInfoGetStat", "cellDiscGameGetBootDiscInfo",
};

}  // namespace

const char* LookupFunctionName(u32 fnid) {
    static const std::unordered_map<u32, const char*> table = [] {
        std::unordered_map<u32, const char*> map;
        for (const char* name : kNames) map.emplace(ComputeFnid(name), name);
        return map;
    }();
    const auto it = table.find(fnid);
    return it == table.end() ? nullptr : it->second;
}

}  // namespace cell::hle
