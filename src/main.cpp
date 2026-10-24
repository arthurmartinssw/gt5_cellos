// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — main.cpp
//
// Front end of the CellOS runtime: load a decrypted PS3 executable (EBOOT.elf), bind its
// imports (HLE modules, LLE PRX such as libsre), mount the guest devices and run the primary
// PPU thread on the reference interpreter until the process exits, exitspawns another
// executable, or stops on something not implemented yet. Nothing here is game specific; the
// end report (and the stall report) are designed to be pasted back for diagnosis.
//
//   usage: gt5_cellos <EBOOT.elf> [options] [-- game args...]   (see PrintUsage)
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "common/log.h"
#include "hle/cell_audio.h"
#include "hle/cell_fs.h"
#include "hle/cell_game.h"
#include "hle/cell_gcm_sys.h"
#include "hle/cell_pad.h"
#include "hle/cell_libprof.h"
#include "hle/cell_spurs.h"
#include "hle/cell_sysmodule.h"
#include "hle/cell_sysutil.h"
#include "hle/cell_usbd.h"
#include "hle/net_offline.h"
#include "hle/hle_registry.h"
#include "hle/sys_prx_for_user.h"
#include "hle/sys_spu_hle.h"
#include "loader/param_sfo.h"
#include "loader/ppu_loader.h"
#include "loader/self.h"
#include "lv2/kernel.h"
#include "lv2/stall_monitor.h"
#include "lv2/sys_fs.h"
#include "lv2/sys_memory.h"
#include "lv2/sys_mmapper.h"
#include "lv2/sys_ppu_thread.h"
#include "lv2/sys_spu.h"
#include "lv2/sys_process.h"
#include "lv2/sys_prx.h"
#include "loader/ppu_linker.h"
#include "lv2/syscall_table.h"
#include "memory/guest_memory.h"
#include "ppu/ppu_code_cache.h"
#include "ppu/ppu_context.h"
#include "ppu/ppu_decoder.h"
#include "ppu/ppu_disasm.h"
#include "ppu/ppu_interpreter.h"
#include "spu/spu_interpreter.h"
#include "spu/spu_run_stats.h"

using namespace cell;

namespace {

struct Options {
    loader::LoaderOptions loader;
    const char* path = nullptr;
    bool run = true;
    bool trace_ppu = false;
    u64 max_instructions = 200'000'000;
    std::vector<std::pair<std::string, std::string>> mounts;  // guest prefix -> host dir
    std::vector<std::string> lle;                             // --lle modules, in order
    std::string title_id;                                     // --title-id (overrides PARAM.SFO)
    std::string param_sfo;                                    // --param-sfo PATH
    u32 stall_seconds = 10;                                   // --stall-report SECONDS (0 = off)
    u32 status_every = 0;                                     // --status-every SECONDS (0 = off)
    u16 max_video_height = 720;                               // --resolution 720|1080
    bool gcm_interrupts = true;                               // --no-gcm-interrupts (diagnostic A/B)
};

void PrintUsage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s <EBOOT.elf> [--no-run] [--max-insns N] [--trace-ppu] [--trace] [--argv0 PATH]\n"
                 "          [--dev-flash DIR] [--dev-bdvd DIR] [--dev-hdd0 DIR] [--dev-hdd1 DIR] [--app-home DIR]\n"
                 "          [--lle MODULE]... [--title-id ID] [--param-sfo PATH] [--stall-report SECONDS]\n"
                 "          [-- game args...]\n"
                 "  --max-insns N         primary thread instruction budget (default 200M, 0 = no limit)\n"
                 "  --log-spu-runs        log every SPU group start/exit (default: first 3 per group, then summaries)\n"
                 "  --spu-summary N       seconds between SPU group run summaries (default 10, 0 = off)\n"
                 "  --resolution 720|1080 best video mode the virtual TV offers (default 720)\n"
                 "  --dev-bdvd DIR        disc root (contains PS3_GAME/); automatic for <root>/PS3_GAME/USRDIR/EBOOT.*\n"
                 "  --lle libsre          load /dev_flash/sys/external/libsre.sprx (decrypted) for real\n"
                 "  --lle path/libsre.prx load that file instead (a decrypted PRX on the host)\n"
                 "  --title-id BCUS98114  title id for sys_process_get_paramsfo (default: from PARAM.SFO)\n"
                 "  --stall-report N      print every thread's state after N s without progress (0 = off)\n"
                 "  --status-every N      print the same report every N s (for hangs in polling loops)\n"
                 "  --no-gcm-interrupts   diagnostic: do not create _gcm_intr_thread; GCM vblank/flip/queue/user\n"
                 "                        handlers are only recorded (the phase 7.6 behaviour)\n",
                 program);
}

void PrintLoadReport(const loader::PpuExecutable& exe, const loader::PpuBootImage& image,
                     const hle::HleRegistry& hle, const loader::PpuLinker& linker) {
    std::printf("\n== executable ==\n");
    if (exe.self.wrapped)
        std::printf("SELF  key revision 0x%04x, self type %u, auth id 0x%016" PRIx64 "\n", exe.self.key_revision,
                    exe.self.self_type, exe.self.auth_id);
    else
        std::printf("plain ELF64 (decrypted), OSABI 0x%02x\n", exe.osabi);
    for (const auto& s : image.segments)
        std::printf("PT_LOAD 0x%08x  filesz 0x%08x  memsz 0x%08x  %c%c%c\n", s.vaddr, s.filesz, s.memsz,
                    (s.flags & loader::PF_R) ? 'R' : '-', (s.flags & loader::PF_W) ? 'W' : '-',
                    (s.flags & loader::PF_X) ? 'X' : '-');

    std::printf("\n== imports ==\n");
    for (const auto& m : image.imports) {
        std::printf("%s (v%u, %zu functions, %u variables)\n", m.name.c_str(), m.version, m.functions.size(),
                    m.variable_count);
        for (const auto& f : m.functions) {
            const hle::HleFunction& h = hle.At(f.hle_index);
            const bool lle = linker.IsLle(m.name, f.nid);
            std::printf("  0x%08X  %-34s %s\n", f.nid, h.name.empty() ? "<unknown name>" : h.name.c_str(),
                        lle ? "[LLE]" : h.implemented ? "[HLE]" : "[stub]");
        }
    }
}

void PrintRegisters(const ppu::PpuThreadContext& c) {
    for (int r = 0; r < 32; r += 4)
        std::printf("r%-2d %016" PRIx64 "  r%-2d %016" PRIx64 "  r%-2d %016" PRIx64 "  r%-2d %016" PRIx64 "\n", r,
                    c.gpr[r], r + 1, c.gpr[r + 1], r + 2, c.gpr[r + 2], r + 3, c.gpr[r + 3]);
    std::printf("lr  %016" PRIx64 "  ctr %016" PRIx64 "  xer %016" PRIx64 "  cr %08x\n", c.lr, c.ctr, c.xer, c.cr);
}

// The last HLE calls / syscalls of the thread that stopped, with arguments and results: the
// usual answer to "where did this null pointer come from?".
void PrintRecentCalls(const std::vector<ppu::CallRecord>& calls, lv2::Kernel& kernel, const hle::HleRegistry& hle) {
    if (calls.empty()) return;
    std::printf("\nlast %zu HLE calls / syscalls of this thread (oldest first):\n", calls.size());
    for (const ppu::CallRecord& c : calls) {
        std::string name;
        if (c.syscall) {
            const char* n = c.id < lv2::kSyscallCount ? kernel.syscalls().At(c.id).name : nullptr;
            if (!n && c.id < lv2::kSyscallCount) n = lv2::KnownSyscallName(c.id);
            char text[96];
            std::snprintf(text, sizeof(text), "syscall %u %s%s", c.id, n ? n : "?",
                          c.id < lv2::kSyscallCount && kernel.syscalls().At(c.id).fn ? "" : " [NOT IMPLEMENTED]");
            name = text;
        } else {
            const hle::HleFunction& f = hle.At(c.id);
            name = f.module + "::" + (f.name.empty() ? "?" : f.name) + (f.implemented ? "" : " [stub]");
        }
        std::printf("  %08x  %-52s (0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ") -> 0x%" PRIx64 "\n",
                    c.pc, name.c_str(), c.args[0], c.args[1], c.args[2], c.args[3], c.result);
    }
}

int PrintEndReport(const ppu::StopInfo& primary_stop, lv2::Kernel& kernel, const hle::HleRegistry& hle,
                   const hle::CellGcmSys& gcm, const hle::CellAudio& audio) {
    const lv2::ProcessEnd end = kernel.process_end();
    lv2::ProcessState& process = kernel.process();
    std::printf("\n== process ended ==\n");
    std::printf("primary thread: %s after %" PRIu64 " instructions\n", ppu::StopReasonName(primary_stop.reason),
                primary_stop.executed);

    int code = 0;
    if (end.fatal) {
        code = 3;
        const ppu::StopInfo& stop = end.stop;
        std::printf("\nFATAL in thread 0x%08x: %s\n", end.thread_id, ppu::StopReasonName(stop.reason));
        std::printf("pc 0x%08x  instruction 0x%08x  %s\n", stop.pc, stop.instruction,
                    ppu::Disassemble(stop.pc, stop.instruction).c_str());
        if (const auto ea = ppu::ExplainEffectiveAddress(stop.instruction, end.context)) std::printf("%s\n", ea->c_str());
        if (stop.reason == ppu::StopReason::DataFault || stop.reason == ppu::StopReason::Alignment) {
            const auto area = vm::GuestMemory::AreaOf(stop.address);
            std::printf("%s at EA 0x%08x (area: %s)\n", stop.write ? "write" : "read", stop.address,
                        area ? vm::Describe(*area).name : "unmapped hole");
        }
        if (!stop.detail.empty()) std::printf("detail: %s\n", stop.detail.c_str());
        if (!end.message.empty()) std::printf("reason: %s\n", end.message.c_str());
        PrintRecentCalls(end.recent_calls, kernel, hle);
        std::printf("\nregisters:\n");
        PrintRegisters(end.context);
        // The path that led here, disassembled from current memory (oldest first).
        std::printf("\nlast %zu instructions before the fault:\n", end.recent_pcs.size());
        const vm::GuestMemory& memory = kernel.memory();
        for (u32 pc : end.recent_pcs) {
            if (!memory.Check(pc, 4, vm::kPageReadable)) continue;
            const u32 word = memory.Read<u32>(pc);
            std::printf("  %08x: %08x  %s\n", pc, word, ppu::Disassemble(pc, word).c_str());
        }
        std::printf("> %08x: %08x  %s\n", stop.pc, stop.instruction, ppu::Disassemble(stop.pc, stop.instruction).c_str());
    } else {
        static constexpr const char* kKinds[] = {"process exit", "thread exit", "exitspawn", "abort"};
        std::printf("cause: %s from thread 0x%08x, status 0x%" PRIx64 "\n", kKinds[end.kind % 4], end.thread_id,
                    end.status);
        if (!end.message.empty()) std::printf("reason: %s\n", end.message.c_str());
        if (end.kind == ppu::kExitAbort) code = 3;
        std::lock_guard lock(process.mutex);
        if (end.kind == ppu::kExitSpawn) {
            std::printf("\nThe executable asked lv2 to replace it with:\n  %s\n", process.exitspawn_path.c_str());
            for (std::size_t i = 0; i < process.exitspawn_args.size(); ++i)
                std::printf("  argv[%zu] = %s\n", i, process.exitspawn_args[i].c_str());
        }
    }

    const auto prx = kernel.prx().Describe();
    if (!prx.empty()) {
        std::printf("\nPRX modules (LLE):\n");
        for (const std::string& line : prx) std::printf("  %s\n", line.c_str());
    }

    std::printf("\nPPU threads still registered:\n");
    for (const auto& t : kernel.threads().Snapshot())
        std::printf("  0x%08x %-24s %s%s exit 0x%" PRIx64 "\n", t.id, t.name.c_str(),
                    t.state == lv2::PpuThread::State::Exited ? "exited  " : "running ",
                    t.primary ? " (primary)" : "", t.exit_value);

    const auto spu = kernel.spu().Describe();
    if (!spu.empty()) {
        std::printf("\nSPU thread groups:\n");
        for (const std::string& line : spu) std::printf("  %s\n", line.c_str());
    }

    std::printf("\nDemand paging (sys_mmapper):\n");
    for (const std::string& line : kernel.mmapper().Describe()) std::printf("  %s\n", line.c_str());

    const auto audio_lines = audio.Describe();
    if (!audio_lines.empty()) {
        std::printf("\ncellAudio:\n");
        for (const std::string& line : audio_lines) std::printf("  %s\n", line.c_str());
    }

    std::printf("\nFile system:\n");
    for (const std::string& line : kernel.fs().Describe()) std::printf("  %s\n", line.c_str());

    const auto rsx = gcm.Describe();
    if (!rsx.empty()) {
        std::printf("\nRSX (cellGcm initialized):\n");
        for (const std::string& line : rsx) std::printf("  %s\n", line.c_str());
    }

    std::printf("\nHLE calls:\n");
    for (u32 i = 0; i < hle.size(); ++i) {
        const hle::HleFunction& f = hle.At(i);
        const u64 calls = f.calls.load();
        if (calls)
            std::printf("  %8" PRIu64 "  %s::%s (0x%08X)%s\n", calls, f.module.c_str(),
                        f.name.empty() ? "?" : f.name.c_str(), f.nid, f.implemented ? "" : "  [stub]");
    }
    return code;
}

// Guest devices: /app_home = the EBOOT's folder; /dev_bdvd = the disc root when the EBOOT sits
// in a disc layout (<root>/PS3_GAME/USRDIR/EBOOT.*); then the explicit --dev-* options, which
// win. /dev_bdvd and /dev_flash are read-only, as on the console.
void MountDevices(const Options& o, lv2::Vfs& vfs) {
    namespace fs = std::filesystem;
    const fs::path eboot_dir = fs::absolute(fs::path(o.path)).parent_path();
    vfs.Mount("/app_home", eboot_dir);
    const fs::path game_dir = eboot_dir.parent_path();
    if (eboot_dir.filename() == "USRDIR" && game_dir.filename() == "PS3_GAME") {
        vfs.Mount("/dev_bdvd", game_dir.parent_path(), true);
        LOG_INFO("main", "disc layout detected: /dev_bdvd -> %s", game_dir.parent_path().string().c_str());
    }
    for (const auto& [guest, host] : o.mounts) vfs.Mount(guest, host, lv2::Vfs::IsReadOnlyDevice(guest));
    for (const lv2::VfsMount& m : vfs.Mounts())
        LOG_INFO("main", "mount %-10s -> %s%s", m.prefix.c_str(), m.host.string().c_str(), m.read_only ? " (read-only)" : "");
    if (!vfs.IsMounted("/dev_hdd1")) {
        // System cache (cellSysCacheMount): scratch space the console wipes at will.
        const fs::path cache = fs::temp_directory_path() / "cellos_dev_hdd1";
        std::error_code ec;
        fs::create_directories(cache, ec);
        vfs.Mount("/dev_hdd1", cache);
        LOG_INFO("main", "mount /dev_hdd1  -> %s (default system cache; --dev-hdd1 to choose)", cache.string().c_str());
    }
    if (!vfs.IsMounted("/dev_hdd0"))
        LOG_WARN("main", "/dev_hdd0 is not mounted: game data / saves / caches cannot be written (pass --dev-hdd0 DIR)");
}

// Title id for sys_process_get_paramsfo: --title-id, else TITLE_ID of the first PARAM.SFO
// found (--param-sfo, the disc layout PS3_GAME/USRDIR/EBOOT.BIN -> PS3_GAME/PARAM.SFO, the
// EBOOT's own folder, /dev_bdvd/PS3_GAME/PARAM.SFO).
// Fills title_id and the PARAM.SFO tables of the process. --title-id wins over the file.
void LoadParamSfo(const Options& o, lv2::Kernel& kernel, lv2::ProcessState& process) {
    std::vector<std::filesystem::path> candidates;
    if (!o.param_sfo.empty()) candidates.emplace_back(o.param_sfo);
    const auto eboot_dir = std::filesystem::absolute(std::filesystem::path(o.path)).parent_path();
    candidates.push_back(eboot_dir.parent_path() / "PARAM.SFO");
    candidates.push_back(eboot_dir / "PARAM.SFO");
    if (const auto bdvd = kernel.vfs().Resolve("/dev_bdvd/PS3_GAME/PARAM.SFO")) candidates.push_back(*bdvd);
    for (const auto& path : candidates) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) continue;
        const auto sfo = loader::ParseParamSfo(loader::ReadWholeFile(path));
        if (!sfo) {
            LOG_WARN("main", "%s is not a valid PARAM.SFO", path.string().c_str());
            continue;
        }
        for (const auto& [key, value] : *sfo) {
            if (value.fmt == 0x0404) process.sfo_int[key] = value.integer;
            else process.sfo_text[key] = value.text;
        }
        process.title_id = process.sfo_text["TITLE_ID"];
        LOG_INFO("main", "PARAM.SFO %s: %s \"%s\" (category %s, version %s)", path.string().c_str(), process.title_id.c_str(),
                 process.sfo_text["TITLE"].c_str(), process.sfo_text["CATEGORY"].c_str(), process.sfo_text["APP_VER"].c_str());
        break;
    }
    if (!o.title_id.empty()) process.title_id = o.title_id;
    if (process.title_id.empty())
        LOG_WARN("main", "no PARAM.SFO found (tried next to the EBOOT and /dev_bdvd): sys_process_get_paramsfo will return "
                         "ENOENT; pass --title-id <ID> (e.g. BCUS98114) if a library needs it");
    // A disc boot when /dev_bdvd is mounted and the EBOOT lives under it.
    const auto bdvd = kernel.vfs().Resolve("/dev_bdvd");
    std::error_code ec;
    process.disc_boot = bdvd && std::filesystem::equivalent(*bdvd, eboot_dir.parent_path().parent_path(), ec);
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--argv0") && i + 1 < argc) o.loader.argv0 = argv[++i];
        else if (!std::strcmp(argv[i], "--max-insns") && i + 1 < argc) {
            o.max_instructions = std::strtoull(argv[++i], nullptr, 0);
            if (!o.max_instructions) o.max_instructions = ~u64{0};  // 0 = no limit
        }
        else if (!std::strcmp(argv[i], "--no-run")) o.run = false;
        else if (!std::strcmp(argv[i], "--trace-ppu")) o.trace_ppu = true;
        else if (!std::strcmp(argv[i], "--trace")) log::Threshold() = log::Level::Trace;
        else if (!std::strcmp(argv[i], "--dev-flash") && i + 1 < argc) o.mounts.emplace_back("/dev_flash", argv[++i]);
        else if (!std::strcmp(argv[i], "--dev-bdvd") && i + 1 < argc) o.mounts.emplace_back("/dev_bdvd", argv[++i]);
        else if (!std::strcmp(argv[i], "--dev-hdd0") && i + 1 < argc) o.mounts.emplace_back("/dev_hdd0", argv[++i]);
        else if (!std::strcmp(argv[i], "--dev-hdd1") && i + 1 < argc) o.mounts.emplace_back("/dev_hdd1", argv[++i]);
        else if (!std::strcmp(argv[i], "--app-home") && i + 1 < argc) o.mounts.emplace_back("/app_home", argv[++i]);
        else if (!std::strcmp(argv[i], "--lle") && i + 1 < argc) o.lle.emplace_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--title-id") && i + 1 < argc) o.title_id = argv[++i];
        else if (!std::strcmp(argv[i], "--log-spu-runs")) spu::RunLogConfig().verbose.store(true);
        else if (!std::strcmp(argv[i], "--spu-summary") && i + 1 < argc)
            spu::RunLogConfig().summary_seconds.store(static_cast<u32>(std::strtoul(argv[++i], nullptr, 0)));
        else if (!std::strcmp(argv[i], "--resolution") && i + 1 < argc)
            o.max_video_height = std::strtoul(argv[++i], nullptr, 0) >= 1080 ? 1080 : 720;
        else if (!std::strcmp(argv[i], "--param-sfo") && i + 1 < argc) o.param_sfo = argv[++i];
        else if (!std::strcmp(argv[i], "--stall-report") && i + 1 < argc)
            o.stall_seconds = static_cast<u32>(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--no-gcm-interrupts")) o.gcm_interrupts = false;
        else if (!std::strcmp(argv[i], "--status-every") && i + 1 < argc)
            o.status_every = static_cast<u32>(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--")) {
            for (++i; i < argc; ++i) o.loader.args.emplace_back(argv[i]);
        } else if (!o.path) o.path = argv[i];
        else {
            PrintUsage(argv[0]);
            return 2;
        }
    }
    if (!o.path) {
        PrintUsage(argv[0]);
        return 2;
    }

    try {
        loader::PpuExecutable exe = loader::ParseExecutable(loader::ReadWholeFile(o.path));

        vm::GuestMemory memory;
        memory.Initialize();

        lv2::ProcessState process;
        lv2::SyscallTable syscalls;
        lv2::MemoryService memory_service(memory);
        memory_service.RegisterSyscalls(syscalls);
        lv2::ProcessService process_service(memory, process);
        process_service.RegisterSyscalls(syscalls);

        hle::HleRegistry hle(memory);
        ppu::PpuCodeCache code(memory);
        lv2::Kernel kernel(memory, hle, syscalls, code, process);
        kernel.RegisterSyscalls();
        // SPU threads run on the reference SPU interpreter (the SPU -> x86-64 JIT plugs in here).
        kernel.spu().SetExecutorFactory(spu::MakeSpuInterpreter);

        // HLE modules register before Load(): imports bind to real handlers.
        hle::SysPrxForUser sys_prx_for_user(memory, process, kernel);
        sys_prx_for_user.Register(hle);
        hle::CellSysmodule cell_sysmodule;
        cell_sysmodule.Register(hle);
        hle::SysSpuHle sys_spu_hle(kernel);
        sys_spu_hle.Register(hle);
        hle::CellSpurs cell_spurs;
        cell_spurs.Register(hle);
        hle::CellLibprof cell_libprof;  // libsre's trace registration
        cell_libprof.Register(hle);
        hle::CellFs cell_fs(kernel);  // sys_fs: cellFs* over the lv2 file system service
        cell_fs.Register(hle);
        hle::CellSysutil cell_sysutil(kernel, {o.max_video_height});  // video/audio out, callbacks, params, cache
        cell_sysutil.Register(hle);
        hle::CellGame cell_game(kernel);  // boot check / content paths / PARAM.SFO
        cell_game.Register(hle);
        hle::NetOffline net_offline;  // cellNetCtl / sceNp / sys_net: no cable, no PSN
        net_offline.Register(hle);
        hle::CellAudio cell_audio(kernel);  // ports + 5.333 ms mixer clock (stops before the kernel goes away)
        cell_audio.Register(hle);
        hle::CellUsbd cell_usbd;  // USB LDD framework, no devices attached
        cell_usbd.Register(hle);
        hle::CellPad cell_pad;  // sys_io: controller API, no controller connected
        cell_pad.Register(hle);
        hle::CellGcmSys cell_gcm_sys(kernel);  // destroyed before the kernel: joins the RSX thread
        cell_gcm_sys.set_interrupts_enabled(o.gcm_interrupts);
        cell_gcm_sys.Register(hle);

        loader::PpuLoader loader(memory, hle, &kernel.prx().linker());
        const loader::PpuBootImage image = loader.Load(exe, o.loader);
        process.sdk_version = image.proc.sdk_version;
        process.tls_vaddr = image.tls.vaddr;
        process.tls_filesz = image.tls.filesz;
        process.tls_memsz = image.tls.memsz;
        process.primary_stack_size = image.proc.primary_stacksize;

        // Guest devices. /app_home defaults to the EBOOT's folder.
        MountDevices(o, kernel.vfs());
        LoadParamSfo(o, kernel, process);

        // Stall/deadlock diagnostics: armed before module_start, which may block too.
        lv2::StallMonitor stall_monitor(kernel, o.run ? o.stall_seconds : 0, o.run ? o.status_every : 0);
        stall_monitor.AddSection("RSX", [&cell_gcm_sys] { return cell_gcm_sys.Describe(); });
        stall_monitor.AddSection("File system", [&kernel] { return kernel.fs().Describe(); });
        stall_monitor.AddSection("Demand paging (sys_mmapper)", [&kernel] { return kernel.mmapper().Describe(); });
        stall_monitor.AddSection("cellAudio", [&cell_audio] { return cell_audio.Describe(); });
        stall_monitor.Start();

        // Boot-time LLE modules: loaded after the EBOOT (the linker re-points its imports to the
        // module's exports), module_start run before the game's entry point.
        for (const std::string& name : o.lle) {
            std::filesystem::path host;
            std::string guest;
            std::error_code ec;
            if (std::filesystem::is_regular_file(name, ec)) {
                host = name;
                guest = "/host/" + std::filesystem::path(name).filename().string();
            } else {
                guest = "/dev_flash/sys/external/" + name;
                if (guest.find(".sprx") == std::string::npos && guest.find(".prx") == std::string::npos) guest += ".sprx";
                const auto resolved = kernel.vfs().Resolve(guest);
                if (!resolved) {
                    LOG_ERROR("main", "--lle %s: no file of that name and no --dev-flash mount for %s", name.c_str(), guest.c_str());
                    return 1;
                }
                host = *resolved;
            }
            u32 id = 0;
            if (kernel.prx().LoadFile(host, guest, &id) != CELL_OK) {
                LOG_ERROR("main", "--lle %s: could not load %s (decrypted PRX expected; see the error above)", name.c_str(),
                          host.string().c_str());
                return 1;
            }
            s32 result = 0;
            if (kernel.prx().StartModule(id, 0, 0, &result) != CELL_OK || kernel.is_terminating()) {
                LOG_ERROR("main", "--lle %s: module_start failed (result %d)", name.c_str(), result);
                kernel.Shutdown();
                stall_monitor.Stop();
                return PrintEndReport(ppu::StopInfo{}, kernel, hle, cell_gcm_sys, cell_audio);
            }
        }
        PrintLoadReport(exe, image, hle, kernel.prx().linker());

        ppu::PpuThreadContext primary;
        loader.InitializePrimaryThread(image, primary);
        if (!o.run) {
            std::printf("\n--no-run: primary thread ready at pc 0x%08x\n", primary.cia);
            return 0;
        }

        std::printf("\n== running primary PPU thread from 0x%08x (interpreter, multi-threaded lv2) ==\n", primary.cia);
        lv2::ThreadManager& threads = kernel.threads();
        threads.AdoptPrimary(primary, image.proc.primary_prio, "main");
        const ppu::StopInfo stop = threads.RunPrimary(o.max_instructions, o.trace_ppu);
        // The primary thread may leave with sys_ppu_thread_exit while others keep running:
        // the process lives until they finish or one of them ends it.
        if (stop.reason == ppu::StopReason::Exited && threads.primary_context().exit_kind == ppu::kExitThread)
            threads.WaitForAllThreads();
        kernel.Shutdown();  // joins PPU and SPU host threads (the stall monitor watches this too)
        stall_monitor.Stop();
        return PrintEndReport(stop, kernel, hle, cell_gcm_sys, cell_audio);
    } catch (const loader::LoadError& e) {
        LOG_ERROR("loader", "%s", e.what());
        return 1;
    } catch (const std::exception& e) {
        LOG_ERROR("main", "%s", e.what());
        return 1;
    }
}
