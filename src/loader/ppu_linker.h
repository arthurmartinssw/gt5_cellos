// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — loader/ppu_linker.h
//
// Dynamic linking of PPU imports, the part lv2's PRX loader does at load time.
//
// Every imported function is a (library name, FNID) pair with a 32-bit slot in the importer's
// data that must hold the OPD of the callee. Two providers exist:
//   * LLE — an export of a loaded PRX module (e.g. libsre.sprx exporting "cellSpurs"): the
//     slot gets the module's own OPD and the call runs real PS3 code;
//   * HLE — anything no loaded module exports: the slot gets an HleRegistry thunk OPD.
// LLE always wins. The linker remembers every slot it bound, so a module loaded later
// re-points the slots of earlier importers (the EBOOT included), and unloading it puts them
// back on HLE — the same relinking lv2 does for sys_prx_load_module at run time.
//
// Thread safety: one mutex; slot writes are plain guest stores done while no PPU thread can
// be executing the patched call (boot) or under the importer's own synchronization (runtime
// load, as on hardware).
#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hle/hle_registry.h"
#include "loader/ppu_loader.h"
#include "memory/guest_memory.h"

namespace cell::loader {

class PpuLinker {
public:
    PpuLinker(vm::GuestMemory& memory, hle::HleRegistry& hle) noexcept : memory_(memory), hle_(hle) {}

    // Walks a libstub table [start, end) (array of ppu_prx_module_info_t), binds every
    // function import and records its slot for `importer`. Throws LoadError on a malformed
    // table. Variables (vnids) are reported, not linked.
    std::vector<ImportedModule> BindImportTable(u32 start, u32 end, u32 importer);

    // Exports of a module: `addr` is the OPD for functions, the variable's EA for variables.
    void AddExports(u32 module, const std::string& library, const std::vector<std::pair<u32, u32>>& functions);
    // Re-points every recorded slot whose (library, nid) now has an LLE export.
    std::size_t RelinkAll();
    // Removes a module: its exports disappear (slots fall back to HLE) and its own slots are forgotten.
    void RemoveModule(u32 module);

    // LLE export for (library, nid), if a loaded module provides one.
    [[nodiscard]] std::optional<u32> FindExport(const std::string& library, u32 nid) const;
    // What a slot currently resolves to: "LLE <module>" / "HLE" (for reports).
    [[nodiscard]] bool IsLle(const std::string& library, u32 nid) const;

    static constexpr u32 kMainExecutable = 0;  // importer id of the EBOOT

private:
    struct Key {
        std::string library;
        u32 nid;
        bool operator<(const Key& o) const { return library != o.library ? library < o.library : nid < o.nid; }
    };
    struct Export {
        u32 module;
        u32 addr;
    };
    struct Slot {
        u32 importer;
        u32 ea;
    };
    u32 TargetLocked(const Key& key);  // export OPD or HLE OPD

    vm::GuestMemory& memory_;
    hle::HleRegistry& hle_;
    mutable std::mutex mutex_;
    std::map<Key, Export> exports_;
    std::map<Key, std::vector<Slot>> slots_;
};

}  // namespace cell::loader
