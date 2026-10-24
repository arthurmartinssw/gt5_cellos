// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/sys_prx.cpp
#include "lv2/sys_prx.h"

#include <cstdio>

#include "common/log.h"
#include "lv2/sys_ppu_thread.h"
#include "lv2/vfs.h"

namespace cell::lv2 {

using State = Lv2Prx::State;

PrxManager::PrxManager(Kernel& kernel)
    : kernel_(kernel), linker_(kernel.memory(), kernel.hle()), loader_(kernel.memory(), linker_) {}

PrxManager::~PrxManager() = default;

std::shared_ptr<Lv2Prx> PrxManager::Find(u32 id) const { return kernel_.objects().Get<Lv2Prx>(id); }

s32 PrxManager::LoadFile(const std::filesystem::path& host_path, const std::string& guest_path, u32* out_id) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(host_path, ec)) {
        LOG_ERROR("prx", "load %s: %s does not exist", guest_path.c_str(), host_path.string().c_str());
        return CELL_ENOENT;
    }
    auto prx = std::make_shared<Lv2Prx>();
    const u32 id = kernel_.objects().Add(kIdPrx, prx);
    try {
        const loader::PpuExecutable exe = loader::ParsePrx(loader::ReadWholeFile(host_path));
        prx->module = loader_.Load(exe, host_path.string(), id);
    } catch (const std::exception& e) {
        kernel_.objects().Remove(id);
        LOG_ERROR("prx", "load %s (%s): %s", guest_path.c_str(), host_path.string().c_str(), e.what());
        return CELL_ENOEXEC;
    }
    prx->name = prx->module.name;
    prx->guest_path = guest_path;
    // What the module will call that nothing implements yet: the first places to look when it
    // misbehaves (each is a logging stub returning CELL_OK).
    std::size_t stubs = 0;
    for (const loader::ImportedModule& im : prx->module.imports)
        for (const loader::ImportedFunction& f : im.functions)
            if (!f.lle_addr && !kernel_.hle().At(f.hle_index).implemented) {
                const std::string& fn = kernel_.hle().At(f.hle_index).name;
                LOG_WARN("prx", "  %s imports %s::%s (0x%08X): no implementation, stub returns CELL_OK", prx->module.name.c_str(),
                         im.name.c_str(), fn.empty() ? "?" : fn.c_str(), f.nid);
                ++stubs;
            }
    if (stubs) LOG_WARN("prx", "  %s: %zu import(s) bound to stubs", prx->module.name.c_str(), stubs);
    {
        std::lock_guard lock(mutex_);
        ids_.push_back(id);
    }
    *out_id = id;
    return CELL_OK;
}

s32 PrxManager::LoadModule(const std::string& guest_path, u32* out_id) {
    const auto host = kernel_.vfs().Resolve(guest_path);
    if (!host) {
        LOG_ERROR("prx", "sys_prx_load_module(\"%s\"): no mount for this path (see --dev-flash / --dev-bdvd / --app-home)",
                  guest_path.c_str());
        return CELL_ENOENT;
    }
    return LoadFile(*host, guest_path, out_id);
}

s32 PrxManager::StartModule(u32 id, u64 args, u32 argp, s32* result) {
    const auto prx = Find(id);
    if (!prx) return CELL_PRX_ERROR_UNKNOWN_MODULE;
    u32 entry = 0;
    {
        std::lock_guard lock(prx->m);
        if (prx->state == State::Started || prx->state == State::Starting) return CELL_PRX_ERROR_ALREADY_STARTED;
        prx->state = State::Starting;
        entry = prx->module.module_start;
    }
    s32 res = SYS_PRX_RESIDENT;
    if (entry) {
        const auto r = kernel_.threads().CallGuest(entry, args, argp, prx->module.name + " module_start");
        if (!r) {
            std::lock_guard lock(prx->m);
            prx->state = State::Loaded;
            return CELL_PRX_ERROR_ERROR;
        }
        res = static_cast<s32>(*r);
    }
    *result = res;
    LOG_INFO("prx", "module_start of \"%s\" returned %d", prx->module.name.c_str(), res);
    if (res == SYS_PRX_NO_RESIDENT) return UnloadModule(id) == CELL_OK ? CELL_OK : CELL_PRX_ERROR_ERROR;
    std::lock_guard lock(prx->m);
    prx->state = res == SYS_PRX_RESIDENT ? State::Started : State::Loaded;
    return CELL_OK;
}

s32 PrxManager::StopModule(u32 id, u64 args, u32 argp, s32* result) {
    const auto prx = Find(id);
    if (!prx) return CELL_PRX_ERROR_UNKNOWN_MODULE;
    u32 entry = 0;
    {
        std::lock_guard lock(prx->m);
        if (prx->state == State::Stopped || prx->state == State::Loaded) return CELL_PRX_ERROR_NOT_STARTED;
        if (prx->state != State::Started) return CELL_PRX_ERROR_CAN_NOT_STOP;
        prx->state = State::Stopping;
        entry = prx->module.module_stop;
    }
    s32 res = SYS_PRX_RESIDENT;
    if (entry) {
        const auto r = kernel_.threads().CallGuest(entry, args, argp, prx->module.name + " module_stop");
        if (!r) {
            std::lock_guard lock(prx->m);
            prx->state = State::Started;
            return CELL_PRX_ERROR_ERROR;
        }
        res = static_cast<s32>(*r);
    }
    *result = res;
    std::lock_guard lock(prx->m);
    prx->state = State::Stopped;
    return CELL_OK;
}

s32 PrxManager::UnloadModule(u32 id) {
    const auto prx = Find(id);
    if (!prx) return CELL_PRX_ERROR_UNKNOWN_MODULE;
    {
        std::lock_guard lock(prx->m);
        if (prx->state == State::Started || prx->state == State::Starting || prx->state == State::Stopping)
            return CELL_PRX_ERROR_NOT_REMOVABLE;
        loader_.Unload(prx->module, id);
    }
    kernel_.objects().Remove(id);
    std::lock_guard lock(mutex_);
    std::erase(ids_, id);
    LOG_INFO("prx", "unloaded \"%s\"", prx->module.name.c_str());
    return CELL_OK;
}

s32 PrxManager::GetModuleIdByName(const std::string& name, u32* out_id) const {
    std::vector<u32> ids;
    {
        std::lock_guard lock(mutex_);
        ids = ids_;
    }
    for (u32 id : ids)
        if (const auto prx = Find(id); prx && prx->module.name == name) {
            *out_id = id;
            return CELL_OK;
        }
    return CELL_PRX_ERROR_UNKNOWN_MODULE;
}

std::vector<u32> PrxManager::ListModules(u32 max) const {
    std::vector<u32> ids;
    {
        std::lock_guard lock(mutex_);
        ids = ids_;
    }
    std::vector<u32> out;
    for (u32 id : ids) {
        if (out.size() >= max) break;
        const auto prx = Find(id);
        if (!prx || prx->guest_path == "/dev_flash/sys/external/liblv2.sprx") continue;
        out.push_back(id);
    }
    return out;
}

s32 PrxManager::GetModuleList(u64 flags, u32 opt) {
    if (!(flags & 0x2)) return CELL_OK;  // RPCS3: nothing requested, nothing written
    vm::GuestMemory& m = kernel_.memory();
    // RPCS3 dereferences the pointer unchecked (an access violation); lv2 reports a bad pointer.
    if (!opt || !m.Check(opt, 0x20, vm::kPageWritable)) return CELL_EFAULT;
    if (m.Read<u64>(opt) != 0x20) {
        LOG_WARN("prx", "_sys_prx_get_module_list: unknown option structure size 0x%llx (nothing written)",
                 static_cast<unsigned long long>(m.Read<u64>(opt)));
        return CELL_OK;
    }
    const u32 max = m.Read<u32>(opt + 12), idlist = m.Read<u32>(opt + 20);
    const std::vector<u32> ids = max ? ListModules(max) : std::vector<u32>{};
    if (!ids.empty() && !m.Check(idlist, static_cast<u32>(ids.size() * 4), vm::kPageWritable)) return CELL_EFAULT;
    for (std::size_t i = 0; i < ids.size(); ++i) m.Write<u32>(idlist + static_cast<u32>(i * 4), ids[i]);
    m.Write<u32>(opt + 16, static_cast<u32>(ids.size()));
    return CELL_OK;
}

std::vector<std::string> PrxManager::Describe() const {
    static constexpr const char* kStates[] = {"loaded", "starting", "started", "stopping", "stopped"};
    std::vector<u32> ids;
    {
        std::lock_guard lock(mutex_);
        ids = ids_;
    }
    std::vector<std::string> out;
    char line[256];
    for (u32 id : ids) {
        const auto prx = Find(id);
        if (!prx) continue;
        std::lock_guard lock(prx->m);
        const loader::PrxModule& m = prx->module;
        std::size_t exported = 0;
        for (const auto& lib : m.exports) exported += lib.functions.size();
        std::snprintf(line, sizeof(line), "0x%08x \"%s\" %s at 0x%08x: %zu export libs (%zu functions), %s", id,
                      m.name.c_str(), kStates[static_cast<int>(prx->state)], m.segments.empty() ? 0u : m.segments[0].addr,
                      m.exports.size(), exported, m.path.c_str());
        out.emplace_back(line);
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// Raw syscalls
// ---------------------------------------------------------------------------------------

// opt: { be64 size; be64 cmd; be64 entry; be64 res; be64 entry2 }
s32 PrxManager::TwoPhase(u32 id, u32 opt, bool start) {
    vm::GuestMemory& m = kernel_.memory();
    if (!m.Check(opt, 32, vm::kPageWritable)) return CELL_EFAULT;
    const auto prx = Find(id);
    if (!prx) return CELL_PRX_ERROR_UNKNOWN_MODULE;
    const u64 size = m.Read<u64>(opt), cmd = m.Read<u64>(opt + 8) & 0xF;
    std::lock_guard lock(prx->m);
    if (cmd == 1) {
        if (start && prx->state != State::Loaded && prx->state != State::Stopped) return CELL_PRX_ERROR_ALREADY_STARTED;
        if (!start && prx->state != State::Started) return CELL_PRX_ERROR_NOT_STARTED;
        prx->state = start ? State::Starting : State::Stopping;
        const u32 entry = start ? prx->module.module_start : prx->module.module_stop;
        m.Write<u64>(opt + 16, entry ? u64{entry} : ~u64{0});
        if (size >= 40 && m.Check(opt + 32, 8, vm::kPageWritable)) m.Write<u64>(opt + 32, ~u64{0});
        return CELL_OK;
    }
    if (cmd == 2) {
        if (prx->state != (start ? State::Starting : State::Stopping)) return CELL_PRX_ERROR_ERROR;
        const s32 res = static_cast<s32>(m.Read<u64>(opt + 24));
        prx->state = start ? (res == SYS_PRX_RESIDENT ? State::Started : State::Loaded) : State::Stopped;
        return CELL_OK;
    }
    return CELL_EINVAL;
}

void PrxManager::RegisterSyscalls(SyscallTable& table) {
    using C = ppu::PpuThreadContext;
    static constexpr auto S = [](void* p) -> PrxManager& { return *static_cast<PrxManager*>(p); };
    static constexpr auto A32 = [](const C& c, int i) { return static_cast<u32>(c.gpr[3 + i]); };

    table.Register(
        480, "_sys_prx_load_module",
        [](void* p, C& c) -> s32 {
            PrxManager& s = S(p);
            const auto path = s.kernel_.memory().ReadCString(A32(c, 0), 1024);
            if (!path) return CELL_EFAULT;
            u32 id = 0;
            const s32 r = s.LoadModule(*path, &id);
            return r == CELL_OK ? static_cast<s32>(id) : r;
        },
        this);
    table.Register(481, "_sys_prx_start_module", [](void* p, C& c) -> s32 { return S(p).TwoPhase(A32(c, 0), A32(c, 2), true); }, this);
    table.Register(482, "_sys_prx_stop_module", [](void* p, C& c) -> s32 { return S(p).TwoPhase(A32(c, 0), A32(c, 2), false); }, this);
    table.Register(483, "_sys_prx_unload_module", [](void* p, C& c) -> s32 { return S(p).UnloadModule(A32(c, 0)); }, this);
    table.Register(494, "_sys_prx_get_module_list", [](void* p, C& c) -> s32 { return S(p).GetModuleList(c.gpr[3], A32(c, 1)); }, this);
    table.Register(
        496, "_sys_prx_get_module_id_by_name",
        [](void* p, C& c) -> s32 {
            PrxManager& s = S(p);
            const auto name = s.kernel_.memory().ReadCString(A32(c, 0), 64);
            if (!name) return CELL_EFAULT;
            u32 id = 0;
            const s32 r = s.GetModuleIdByName(*name, &id);
            return r == CELL_OK ? static_cast<s32>(id) : r;
        },
        this);
}

}  // namespace cell::lv2
