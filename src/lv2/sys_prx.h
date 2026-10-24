// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_prx.h
//
// lv2 PRX modules: load real PS3 modules (LLE) into the process and link them.
//
// Raw syscalls (lv2 numbering, as in RPCS3's table):
//   480 _sys_prx_load_module(path, flags, opt)          -> module id (or error) in r3
//   481 _sys_prx_start_module(id, flags, opt)           two-phase, see below
//   482 _sys_prx_stop_module(id, flags, opt)            two-phase
//   483 _sys_prx_unload_module(id, flags, opt)
//   494 _sys_prx_get_module_list(u64 flags, sys_prx_get_module_list_option_t* opt)
//        opt: { be64 size (0x20); be32 pad; be32 max; be32 count; be32 idlist*; be32 unk }.
//        RPCS3 lv2/sys_prx.cpp: nothing unless flags & 2; size != 0x20 -> nothing written;
//        otherwise writes up to `max` module ids to idlist (every loaded PRX except
//        /dev_flash/sys/external/liblv2.sprx) and the number written to count; always CELL_OK.
//   496 _sys_prx_get_module_id_by_name(name, flags, opt) -> module id
// Two-phase start/stop (liblv2's user-side wrapper drives it): opt = sys_prx_start_stop_
// module_option_t { be64 size; be64 cmd; be64 entry; be64 res; be64 entry2 }:
//   cmd 1: lv2 returns the entry point (module_start / module_stop OPD, ~0 if none) in entry;
//   the caller runs it on its own thread; cmd 2: the caller reports its result in res
//   (0 SYS_PRX_RESIDENT -> started / stopped, 1 SYS_PRX_NO_RESIDENT -> unloaded).
// The user-side functions games import from sysPrxForUser (sys_prx_load_module,
// sys_prx_start_module(id, args, argp, int* modres, flags, opt), ...) are HLE on top of
// PrxManager and run module_start themselves (ThreadManager::CallGuest).
//
// Boot-time LLE (main.cpp --lle): the module is loaded after the EBOOT, so the linker
// re-points the EBOOT's imports (e.g. every cellSpurs* slot) at the module's exports, and
// module_start runs before the game's entry point, as lv2 does for its preloaded modules.
#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "loader/ppu_linker.h"
#include "loader/prx_loader.h"
#include "lv2/kernel.h"

namespace cell::lv2 {

inline constexpr u32 kIdPrx = 0x23000000;

// CELL_PRX_ERROR_* (values as RPCS3 defines them).
inline constexpr s32 CELL_PRX_ERROR_ERROR = static_cast<s32>(0x80011001u);
inline constexpr s32 CELL_PRX_ERROR_UNKNOWN_MODULE = static_cast<s32>(0x8001112Eu);
inline constexpr s32 CELL_PRX_ERROR_ALREADY_STARTED = static_cast<s32>(0x80011133u);
inline constexpr s32 CELL_PRX_ERROR_NOT_STARTED = static_cast<s32>(0x80011134u);
inline constexpr s32 CELL_PRX_ERROR_ALREADY_STOPPED = static_cast<s32>(0x80011135u);
inline constexpr s32 CELL_PRX_ERROR_CAN_NOT_STOP = static_cast<s32>(0x80011136u);
inline constexpr s32 CELL_PRX_ERROR_NOT_REMOVABLE = static_cast<s32>(0x80011138u);

inline constexpr s32 SYS_PRX_RESIDENT = 0;
inline constexpr s32 SYS_PRX_NO_RESIDENT = 1;

struct Lv2Prx : Lv2Object {
    enum class State { Loaded, Starting, Started, Stopping, Stopped };
    std::mutex m;
    State state = State::Loaded;
    loader::PrxModule module;
    std::string guest_path;  // path it was loaded as (/dev_flash/..., /host/... for --lle files)
};

class PrxManager {
public:
    explicit PrxManager(Kernel& kernel);
    ~PrxManager();

    loader::PpuLinker& linker() noexcept { return linker_; }

    // Loading: from a host file (boot-time --lle) or a guest path resolved through the VFS.
    s32 LoadFile(const std::filesystem::path& host_path, const std::string& guest_path, u32* out_id);
    s32 LoadModule(const std::string& guest_path, u32* out_id);
    // Runs module_start(args, argp) / module_stop on a PPU thread and waits; *result = its r3.
    s32 StartModule(u32 id, u64 args, u32 argp, s32* result);
    s32 StopModule(u32 id, u64 args, u32 argp, s32* result);
    s32 UnloadModule(u32 id);
    s32 GetModuleIdByName(const std::string& name, u32* out_id) const;
    // Ids of the loaded modules in load order, liblv2.sprx excluded, at most `max` (the list
    // _sys_prx_get_module_list returns). Only modules loaded as PRX objects are listed: the
    // HLE libraries of this runtime are not PRXes (RPCS3 has the same property for its HLE
    // modules), so the list is shorter than on a console.
    [[nodiscard]] std::vector<u32> ListModules(u32 max) const;
    // Syscall 494's work on guest memory (also used by the sysPrxForUser wrapper).
    s32 GetModuleList(u64 flags, u32 opt);
    [[nodiscard]] std::shared_ptr<Lv2Prx> Find(u32 id) const;

    [[nodiscard]] std::vector<std::string> Describe() const;
    void RegisterSyscalls(SyscallTable& table);

private:
    s32 TwoPhase(u32 id, u32 opt, bool start);

    Kernel& kernel_;
    loader::PpuLinker linker_;
    loader::PrxLoader loader_;
    mutable std::mutex mutex_;
    std::vector<u32> ids_;  // load order, for reports
};

}  // namespace cell::lv2
