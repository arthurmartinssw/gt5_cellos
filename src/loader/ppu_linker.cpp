// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — loader/ppu_linker.cpp
#include "loader/ppu_linker.h"

#include <cstdio>

#include "common/log.h"

namespace cell::loader {

namespace {
template <typename... Args>
[[noreturn]] void Fail(const char* format, Args... args) {
    char message[512];
    if constexpr (sizeof...(Args) == 0) std::snprintf(message, sizeof(message), "%s", format);
    else std::snprintf(message, sizeof(message), format, args...);
    throw LoadError(message);
}
}  // namespace

u32 PpuLinker::TargetLocked(const Key& key) {
    if (const auto it = exports_.find(key); it != exports_.end()) return it->second.addr;
    const u32 index = hle_.Bind(key.library, key.nid);
    hle_.Materialize();  // idempotent; places the thunk if this import is new
    return hle_.OpdAddress(index);
}

std::vector<ImportedModule> PpuLinker::BindImportTable(u32 start, u32 end, u32 importer) {
    if (end < start || !memory_.Check(start, end - start, vm::kPageReadable))
        Fail("libstub table [0x%08x, 0x%08x) is invalid", start, end);
    std::vector<ImportedModule> out;
    std::lock_guard lock(mutex_);
    for (u32 addr = start; addr < end;) {
        if (end - addr < sizeof(ppu_prx_module_info_t)) Fail("truncated libstub entry at 0x%08x", addr);
        const auto stub = memory_.ReadStruct<ppu_prx_module_info_t>(addr);
        if (stub.size < sizeof(ppu_prx_module_info_t)) Fail("libstub entry at 0x%08x has size 0x%x", addr, stub.size);
        const auto name = memory_.ReadCString(stub.name, 128);
        if (!name || name->empty()) Fail("libstub entry at 0x%08x has an unreadable library name", addr);

        ImportedModule module;
        module.name = *name;
        module.version = stub.version;
        module.attributes = stub.attributes;
        module.variable_count = stub.num_var;
        if (stub.num_var)
            LOG_WARN("loader", "%s imports %u variable(s) that are NOT linked yet: reads through them return 0",
                     name->c_str(), static_cast<u32>(stub.num_var));
        const u32 count = stub.num_func;
        if (count) {
            if (!memory_.Check(stub.nids, count * 4u, vm::kPageReadable) ||
                !memory_.Check(stub.addrs, count * 4u, vm::kPageMapped))
                Fail("import tables of %s are outside loaded segments", module.name.c_str());
            for (u32 i = 0; i < count; ++i) {
                const u32 nid = memory_.Read<u32>(stub.nids + i * 4);
                const u32 slot = stub.addrs + i * 4;
                const Key key{module.name, nid};
                const u32 target = TargetLocked(key);
                memory_.Write<u32>(slot, target);
                slots_[key].push_back({importer, slot});
                const auto ex = exports_.find(key);
                module.functions.push_back({nid, slot, ex == exports_.end() ? hle_.Bind(module.name, nid) : 0,
                                            ex == exports_.end() ? 0u : ex->second.addr});
            }
        }
        out.push_back(std::move(module));
        addr += stub.size;
    }
    return out;
}

void PpuLinker::AddExports(u32 module, const std::string& library, const std::vector<std::pair<u32, u32>>& functions) {
    std::lock_guard lock(mutex_);
    for (const auto& [nid, addr] : functions) {
        const Key key{library, nid};
        if (const auto it = exports_.find(key); it != exports_.end() && it->second.module != module)
            LOG_WARN("loader", "%s::0x%08X exported again by module 0x%x (was 0x%x): the newer one wins",
                     library.c_str(), nid, module, it->second.module);
        exports_[key] = {module, addr};
    }
}

std::size_t PpuLinker::RelinkAll() {
    std::lock_guard lock(mutex_);
    std::size_t changed = 0;
    for (const auto& [key, slots] : slots_) {
        const u32 target = TargetLocked(key);
        for (const Slot& s : slots) {
            if (memory_.Read<u32>(s.ea) != target) {
                memory_.Write<u32>(s.ea, target);
                ++changed;
            }
        }
    }
    return changed;
}

void PpuLinker::RemoveModule(u32 module) {
    {
        std::lock_guard lock(mutex_);
        for (auto it = exports_.begin(); it != exports_.end();) it = it->second.module == module ? exports_.erase(it) : std::next(it);
        for (auto it = slots_.begin(); it != slots_.end();) {
            std::erase_if(it->second, [&](const Slot& s) { return s.importer == module; });
            it = it->second.empty() ? slots_.erase(it) : std::next(it);
        }
    }
    RelinkAll();
}

std::optional<u32> PpuLinker::FindExport(const std::string& library, u32 nid) const {
    std::lock_guard lock(mutex_);
    const auto it = exports_.find({library, nid});
    if (it == exports_.end()) return std::nullopt;
    return it->second.addr;
}

bool PpuLinker::IsLle(const std::string& library, u32 nid) const { return FindExport(library, nid).has_value(); }

}  // namespace cell::loader
