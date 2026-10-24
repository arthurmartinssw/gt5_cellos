// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — tests/test_machine.h
//
// A complete guest environment for tests: arena, lv2 kernel, HLE modules, code at 0x10000
// (64 KiB), data at 0x20000 (64 KiB), a primary-thread stack. Programs run either on a bare
// interpreter (single-thread instruction tests) or as the primary thread of the kernel
// (multi-threaded tests).
#pragma once

#include <cstdio>
#include <memory>
#include <vector>

#include "hle/cell_sysmodule.h"
#include "hle/hle_registry.h"
#include "hle/sys_prx_for_user.h"
#include "lv2/kernel.h"
#include "lv2/sys_memory.h"
#include "lv2/sys_ppu_thread.h"
#include "lv2/sys_process.h"
#include "lv2/syscall_table.h"
#include "memory/guest_memory.h"
#include "ppu/ppu_code_cache.h"
#include "ppu/ppu_interpreter.h"

inline int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        const unsigned long long va_ = static_cast<unsigned long long>(a), vb_ = static_cast<unsigned long long>(b); \
        if (va_ != vb_) {                                                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s == %s (0x%llx != 0x%llx)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            ++g_failures;                                                                                  \
        }                                                                                                  \
    } while (0)

constexpr cell::u32 kCode = 0x10000, kData = 0x20000;

struct Machine {
    cell::vm::GuestMemory memory;
    cell::lv2::ProcessState process;
    cell::lv2::SyscallTable syscalls;
    std::unique_ptr<cell::lv2::MemoryService> memory_service;
    std::unique_ptr<cell::lv2::ProcessService> process_service;
    std::unique_ptr<cell::hle::HleRegistry> hle;
    std::unique_ptr<cell::ppu::PpuCodeCache> code;
    std::unique_ptr<cell::lv2::Kernel> kernel;
    std::unique_ptr<cell::hle::SysPrxForUser> prx;
    std::unique_ptr<cell::hle::CellSysmodule> sysmodule;
    std::unique_ptr<cell::ppu::PpuInterpreter> cpu;
    cell::ppu::PpuThreadContext ctx{};
    cell::u32 add_opd = 0;  // OPD of the test HLE function "test_add" (r3 = r3 + r4)
    cell::u32 stack_base = 0;
    cell::u32 stack_top = 0;
    bool code_writable = false;

    explicit Machine(bool writable_code = false) : code_writable(writable_code) {
        using namespace cell;
        memory.Initialize();
        // Image first (fixed addresses), then the HLE thunks: same order as PpuLoader::Load.
        CHECK(memory.AllocateFixed(vm::Area::Main, kCode, vm::k64K, vm::kProtReadWrite));
        CHECK(memory.AllocateFixed(vm::Area::Main, kData, vm::k64K, vm::kProtReadWrite));
        memory_service = std::make_unique<lv2::MemoryService>(memory);
        memory_service->RegisterSyscalls(syscalls);
        process_service = std::make_unique<lv2::ProcessService>(memory, process);
        process_service->RegisterSyscalls(syscalls);
        hle = std::make_unique<hle::HleRegistry>(memory);
        code = std::make_unique<ppu::PpuCodeCache>(memory);
        kernel = std::make_unique<lv2::Kernel>(memory, *hle, syscalls, *code, process);
        kernel->RegisterSyscalls();
        prx = std::make_unique<hle::SysPrxForUser>(memory, process, *kernel);
        prx->Register(*hle);
        sysmodule = std::make_unique<hle::CellSysmodule>();
        sysmodule->Register(*hle);
        hle->Register("test", 0x1234, "test_add", [](ppu::PpuThreadContext& c, vm::GuestMemory&, const hle::HleFunction&) {
            c.gpr[3] = c.gpr[3] + c.gpr[4];
        });
        const u32 add_index = hle->Bind("test", 0x1234);
        hle->Materialize();
        add_opd = hle->OpdAddress(add_index);
        const auto stack = memory.Allocate(vm::Area::Stack, vm::k64K * 4, vm::k64K, vm::kProtReadWrite);
        stack_base = *stack;
        stack_top = *stack + vm::k64K * 4;
        cpu = std::make_unique<ppu::PpuInterpreter>(memory, *hle, syscalls, *code);
    }
    ~Machine() {
        if (kernel) kernel->Shutdown();
    }

    // OPD of an HLE import, bound the way the loader would.
    cell::u32 ImportOpd(const char* module, const char* name) {
        const cell::u32 index = hle->Bind(module, cell::hle::ComputeFnid(name));
        hle->Materialize();
        return hle->OpdAddress(index);
    }

    void Load(const std::vector<cell::u32>& program, cell::u32 at = kCode) {
        for (std::size_t i = 0; i < program.size(); ++i) memory.Write<cell::u32>(at + static_cast<cell::u32>(i * 4), program[i]);
    }

    void PrepareContext() {
        ctx = cell::ppu::PpuThreadContext{};
        ctx.gpr[1] = stack_top - 0x70;
        ctx.lr = hle->CodeAddress(hle->exit_index());
        ctx.cia = kCode;
        ctx.thread_id = 0x01000000;
        ctx.vscr = 0x10000;
        ctx.stack_addr = stack_base;
        ctx.stack_size = stack_top - stack_base;
    }

    // Single-threaded: bare interpreter on the test thread.
    cell::ppu::StopInfo Run(const std::vector<cell::u32>& program, cell::u64 max = 100000) {
        Load(program);
        if (!code_writable) memory.Protect(kCode, cell::vm::k64K, cell::vm::kProtRead);
        cpu->InvalidateCode(kCode, cell::vm::k64K);
        PrepareContext();
        return cpu->Execute(ctx, max);
    }

    // Multi-threaded: program runs as the kernel's primary thread; returns when the process
    // has ended and every host thread is joined.
    cell::ppu::StopInfo RunProcess(const std::vector<cell::u32>& program, cell::u64 max = 50'000'000) {
        Load(program);
        if (!code_writable) memory.Protect(kCode, cell::vm::k64K, cell::vm::kProtRead);
        PrepareContext();
        auto& threads = kernel->threads();
        threads.AdoptPrimary(ctx, 1001, "main");
        const auto stop = threads.RunPrimary(max, false);
        if (stop.reason == cell::ppu::StopReason::Exited && threads.primary_context().exit_kind == cell::ppu::kExitThread)
            threads.WaitForAllThreads();
        kernel->Shutdown();
        ctx = threads.primary_context();
        return stop;
    }
};
