// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — ppu/ppu_interpreter.cpp
//
// Semantics follow PowerPC Architecture Book I (v2.02) in 64-bit mode (MSR[SF]=1, which is
// how lv2 runs games) with 32-bit effective addresses: every EA and branch target is
// truncated to 32 bits, as on the PS3. Register notation in comments: (RA|0) means "0 when
// the RA field is 0", EXTS = sign extension, || = concatenation.
#include "ppu/ppu_interpreter.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>

#include "common/log.h"
#include "ppu/ppu_decoder.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace cell::ppu {

namespace {

constexpr u64 kXerSO = 0x80000000u;
constexpr u64 kXerOV = 0x40000000u;
constexpr u64 kXerCA = 0x20000000u;
constexpr u32 kCrLT = 8, kCrGT = 4, kCrEQ = 2, kCrSO = 1;
constexpr u32 kCacheLine = 128;  // Cell PPU L1/L2 line: dcbz granule and reservation granule

inline u64 MulHighU64(u64 a, u64 b) noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
    u64 high;
    _umul128(a, b, &high);
    return high;
#else
    __extension__ typedef unsigned __int128 u128;
    return static_cast<u64>((static_cast<u128>(a) * b) >> 64);
#endif
}
inline s64 MulHighS64(s64 a, s64 b) noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
    s64 high;
    _mul128(a, b, &high);
    return high;
#else
    __extension__ typedef __int128 s128;
    return static_cast<s64>((static_cast<s128>(a) * b) >> 64);
#endif
}

// a + b + carry_in with carry out of bit 0 (IBM) = bit 63 (host).
inline u64 AddCarry(u64 a, u64 b, bool carry_in, bool& carry_out) noexcept {
    const u64 sum = a + b;
    const u64 result = sum + (carry_in ? 1 : 0);
    carry_out = (sum < a) || (result < sum);
    return result;
}
inline bool AddOverflow(u64 a, u64 b, u64 result) noexcept { return ((a ^ result) & (b ^ result)) >> 63; }

// ---- VMX element helpers (v128 holds big-endian element order) ----
inline u32 VW(const v128& v, int i) noexcept {
    u32 x;
    std::memcpy(&x, v.bytes + 4 * i, 4);
    return ByteSwap(x);
}
inline void VSetW(v128& v, int i, u32 x) noexcept {
    x = ByteSwap(x);
    std::memcpy(v.bytes + 4 * i, &x, 4);
}
inline u16 VH(const v128& v, int i) noexcept {
    u16 x;
    std::memcpy(&x, v.bytes + 2 * i, 2);
    return ByteSwap(x);
}
inline void VSetH(v128& v, int i, u16 x) noexcept {
    x = ByteSwap(x);
    std::memcpy(v.bytes + 2 * i, &x, 2);
}
inline f32 VF(const v128& v, int i) noexcept { return std::bit_cast<f32>(VW(v, i)); }
inline void VSetF(v128& v, int i, f32 f) noexcept { VSetW(v, i, std::bit_cast<u32>(f)); }

inline u64 DoubleBits(f64 d) noexcept { return std::bit_cast<u64>(d); }
inline f64 BitsDouble(u64 b) noexcept { return std::bit_cast<f64>(b); }

}  // namespace

const char* StopReasonName(StopReason reason) noexcept {
    switch (reason) {
    case StopReason::None: return "none";
    case StopReason::Exited: return "exited";
    case StopReason::Unimplemented: return "unimplemented instruction";
    case StopReason::InstrFault: return "instruction fetch fault";
    case StopReason::DataFault: return "data access fault";
    case StopReason::Alignment: return "alignment fault";
    case StopReason::Trap: return "trap";
    case StopReason::Privileged: return "privileged instruction";
    case StopReason::StepLimit: return "instruction limit reached";
    case StopReason::Terminated: return "terminated (process exit from another thread)";
    }
    return "?";
}

u64 PpuInterpreter::TimeBase() noexcept {
    static const auto start = std::chrono::steady_clock::now();
    const u64 ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    // ns * 79.8e6 / 1e9 = ns * 798 / 10000, split to avoid overflow.
    return (ns / 10000) * 798 + ((ns % 10000) * 798) / 10000;
}

// =======================================================================================
// Memory access
// =======================================================================================

bool PpuInterpreter::Accessible(u32 ea, u32 size, u8 need) const noexcept {
    if ((memory_.Flags(ea) & need) != need) return false;
    const u64 last = u64{ea} + size - 1;
    if (last > 0xFFFFFFFFull) return false;
    if ((last >> vm::kPageShift) != (ea >> vm::kPageShift))  // straddles two pages
        return (memory_.Flags(static_cast<u32>(last)) & need) == need;
    return true;
}

bool PpuInterpreter::RecoverFault(PpuThreadContext& ctx, u32 ea, u32 size, u8 need, bool write) {
    if (!pf_handler_ || u64{ea} + size - 1 > 0xFFFFFFFFull) return false;
    for (;;) {
        // The first byte that cannot be accessed: `ea`, or the start of the second page.
        const u32 at = (memory_.Flags(ea) & need) == need ? AlignDown<u32>(static_cast<u32>(u64{ea} + size - 1), vm::kPageSize) : ea;
        if (live_) {
            live_->pc.store(ctx.cia, std::memory_order_relaxed);
            live_->args[0].store(at, std::memory_order_relaxed);
            live_->args[1].store(write, std::memory_order_relaxed);
            live_->kind.store(PpuLiveStatus::kInPageFault, std::memory_order_release);
        }
        const bool retry = pf_handler_(pf_user_, ctx, at, write);
        if (live_) live_->kind.store(PpuLiveStatus::kRunning, std::memory_order_release);
        if (!retry) {
            if (kill_ && kill_->load(std::memory_order_relaxed)) Stop(ctx, StopReason::Terminated);
            return false;
        }
        if (Accessible(ea, size, need)) return true;
    }
}

void PpuInterpreter::NoteCodeWrite(u32 ea, u32 size) noexcept {
    // Self-modifying code / code loaded at run time: drop the predecoded copy.
    if ((memory_.Flags(ea) & vm::kPageCode) || (memory_.Flags(ea + size - 1) & vm::kPageCode))
        InvalidateCode(ea, size);
}

namespace {
// Naturally aligned scalars go through relaxed atomic_ref: other PPU threads (and SPU DMA,
// RSX later) touch the same memory concurrently. On x86-64 these compile to plain MOVs.
template <typename T>
inline T LoadGuest(u8* p) noexcept {
    if ((reinterpret_cast<std::uintptr_t>(p) & (sizeof(T) - 1)) == 0)
        return std::atomic_ref<T>(*reinterpret_cast<T*>(p)).load(std::memory_order_relaxed);
    T v{};
    std::memcpy(&v, p, sizeof(T));
    return v;
}
template <typename T>
inline void StoreGuest(u8* p, T v) noexcept {
    if ((reinterpret_cast<std::uintptr_t>(p) & (sizeof(T) - 1)) == 0)
        std::atomic_ref<T>(*reinterpret_cast<T*>(p)).store(v, std::memory_order_relaxed);
    else
        std::memcpy(p, &v, sizeof(T));
}
}  // namespace

template <typename T>
bool PpuInterpreter::Read(PpuThreadContext& ctx, u32 ea, T& out) noexcept {
    if (!Accessible(ea, sizeof(T), vm::kPageReadable) && !RecoverFault(ctx, ea, sizeof(T), vm::kPageReadable, false)) [[unlikely]] {
        Stop(ctx, StopReason::DataFault, ea, false);
        return false;
    }
    out = ByteSwap(LoadGuest<T>(base_ + ea));  // big-endian guest -> host
    return true;
}

template <typename T>
bool PpuInterpreter::Write(PpuThreadContext& ctx, u32 ea, T value) noexcept {
    if (!Accessible(ea, sizeof(T), vm::kPageWritable) && !RecoverFault(ctx, ea, sizeof(T), vm::kPageWritable, true)) [[unlikely]] {
        Stop(ctx, StopReason::DataFault, ea, true);
        return false;
    }
    NoteCodeWrite(ea, sizeof(T));
    StoreGuest<T>(base_ + ea, ByteSwap(value));  // host -> big-endian guest
    return true;
}

bool PpuInterpreter::ReadBytes(PpuThreadContext& ctx, u32 ea, void* out, u32 size) noexcept {
    if (!Accessible(ea, size, vm::kPageReadable) && !RecoverFault(ctx, ea, size, vm::kPageReadable, false)) {
        Stop(ctx, StopReason::DataFault, ea, false);
        return false;
    }
    std::memcpy(out, base_ + ea, size);
    return true;
}

bool PpuInterpreter::WriteBytes(PpuThreadContext& ctx, u32 ea, const void* in, u32 size) noexcept {
    if (!Accessible(ea, size, vm::kPageWritable) && !RecoverFault(ctx, ea, size, vm::kPageWritable, true)) {
        Stop(ctx, StopReason::DataFault, ea, true);
        return false;
    }
    NoteCodeWrite(ea, size);
    std::memcpy(base_ + ea, in, size);
    return true;
}

void PpuInterpreter::Stop(PpuThreadContext& ctx, StopReason reason, u32 address, bool write, std::string detail) {
    if (stopped_) return;  // first cause wins (a page-fault wait cut short by termination stops as Terminated)
    stopped_ = true;
    stop_.reason = reason;
    stop_.pc = ctx.cia;
    stop_.instruction = current_raw_;
    stop_.address = address;
    stop_.write = write;
    stop_.detail = std::move(detail);
}

// =======================================================================================
// Instruction handlers
// =======================================================================================

struct Ops {
    using P = PpuInterpreter;
    using C = PpuThreadContext;

    // ---------------- helpers ----------------
    static u64 RA0(const C& c, Op op) noexcept { return op.ra() ? c.gpr[op.ra()] : 0; }
    static u32 EaD(const C& c, Op op) noexcept { return static_cast<u32>(RA0(c, op) + op.simm16()); }
    static u32 EaDS(const C& c, Op op) noexcept { return static_cast<u32>(RA0(c, op) + op.ds()); }
    static u32 EaX(const C& c, Op op) noexcept { return static_cast<u32>(RA0(c, op) + c.gpr[op.rb()]); }
    static u32 EaDU(const C& c, Op op) noexcept { return static_cast<u32>(c.gpr[op.ra()] + op.simm16()); }
    static u32 EaDSU(const C& c, Op op) noexcept { return static_cast<u32>(c.gpr[op.ra()] + op.ds()); }
    static u32 EaXU(const C& c, Op op) noexcept { return static_cast<u32>(c.gpr[op.ra()] + c.gpr[op.rb()]); }

    static void SetCrField(C& c, u32 field, u32 value) noexcept {
        const u32 shift = 28 - 4 * field;
        c.cr = (c.cr & ~(0xFu << shift)) | ((value & 0xF) << shift);
    }
    static u32 CrBit(const C& c, u32 bit) noexcept { return (c.cr >> (31 - bit)) & 1; }
    static void SetCrBit(C& c, u32 bit, u32 value) noexcept {
        const u32 mask = 1u << (31 - bit);
        c.cr = value ? (c.cr | mask) : (c.cr & ~mask);
    }
    static u32 So(const C& c) noexcept { return (c.xer & kXerSO) ? kCrSO : 0; }
    // Rc=1: CR0 from the 64-bit result (64-bit mode), plus XER[SO].
    static void Cr0(C& c, u64 result) noexcept {
        const s64 r = static_cast<s64>(result);
        SetCrField(c, 0, (r < 0 ? kCrLT : r > 0 ? kCrGT : kCrEQ) | So(c));
    }
    template <typename T>
    static void Compare(C& c, u32 field, T a, T b) noexcept {
        SetCrField(c, field, (a < b ? kCrLT : a > b ? kCrGT : kCrEQ) | So(c));
    }
    static void SetCa(C& c, bool ca) noexcept { c.xer = ca ? (c.xer | kXerCA) : (c.xer & ~kXerCA); }
    static bool Ca(const C& c) noexcept { return (c.xer & kXerCA) != 0; }
    static void SetOv(C& c, bool ov) noexcept { c.xer = ov ? (c.xer | kXerOV | kXerSO) : (c.xer & ~kXerOV); }
    static void FpRc(C& c, Op op) noexcept {
        if (op.rc()) SetCrField(c, 1, c.fpscr >> 28);  // CR1 <- FX FEX VX OX
    }
    static bool TrapCond(u32 to, s64 a, s64 b, u64 ua, u64 ub) noexcept {
        return ((to & 0x10) && a < b) || ((to & 0x08) && a > b) || ((to & 0x04) && a == b) ||
               ((to & 0x02) && ua < ub) || ((to & 0x01) && ua > ub);
    }
    static void DoTrap(P& p, C& c) { p.Stop(c, StopReason::Trap, 0, false, "trap condition met (guest assert?)"); }

    // Integer loads/stores. Returns false when the access faulted (state not committed).
    template <typename T, bool kSigned = false>
    static bool LoadGpr(P& p, C& c, u32 rt, u32 ea) noexcept {
        T value{};
        if (!p.Read(c, ea, value)) return false;
        if constexpr (kSigned) c.gpr[rt] = static_cast<u64>(static_cast<s64>(static_cast<std::make_signed_t<T>>(value)));
        else c.gpr[rt] = value;
        return true;
    }
    template <typename T>
    static bool StoreGpr(P& p, C& c, u32 rs, u32 ea) noexcept {
        return p.Write(c, ea, static_cast<T>(c.gpr[rs]));
    }
    static bool LoadF32(P& p, C& c, u32 frt, u32 ea) noexcept {
        u32 bits = 0;
        if (!p.Read(c, ea, bits)) return false;
        c.fpr[frt] = static_cast<f64>(std::bit_cast<f32>(bits));
        return true;
    }
    static bool LoadF64(P& p, C& c, u32 frt, u32 ea) noexcept {
        u64 bits = 0;
        if (!p.Read(c, ea, bits)) return false;
        c.fpr[frt] = BitsDouble(bits);
        return true;
    }
    static bool StoreF32(P& p, C& c, u32 frs, u32 ea) noexcept {
        return p.Write(c, ea, std::bit_cast<u32>(static_cast<f32>(c.fpr[frs])));
    }
    static bool StoreF64(P& p, C& c, u32 frs, u32 ea) noexcept { return p.Write(c, ea, DoubleBits(c.fpr[frs])); }

    // Branch condition (BO/BI). Decrements CTR when BO[2] is clear.
    static bool BranchTaken(C& c, u32 bo, u32 bi) noexcept {
        if (!(bo & 0x04)) --c.ctr;
        const bool ctr_ok = (bo & 0x04) || ((c.ctr != 0) != ((bo & 0x02) != 0));
        const bool cond_ok = (bo & 0x10) || (CrBit(c, bi) == ((bo >> 3) & 1));
        return ctr_ok && cond_ok;
    }

    // =========================== special ===========================
    static void UNK(P& p, C& c, Op op) {
        char text[160];
        std::snprintf(text, sizeof(text), "word 0x%08X (%s)", op.raw, DescribeEncoding(op.raw).c_str());
        p.Stop(c, StopReason::Unimplemented, 0, false, text);
    }
    static void HLE(P& p, C& c, Op op) {
        // Our thunk: 0x04000000 | index, followed by blr. Arguments in r3.., result in r3.
        u32 index = 0;
        if (!hle::DecodeHleOpcode(op.raw, &index) || index >= p.hle_.size()) return UNK(p, c, op);
        CallRecord& rec = p.calls_[p.call_count_++ & 15];
        rec = {c.cia, false, index, {c.gpr[3], c.gpr[4], c.gpr[5], c.gpr[6]}, 0};
        const u32 call_lr = static_cast<u32>(c.lr);  // the HLE function may change LR (none does today)
        p.EnterCall(c, PpuLiveStatus::kInHle, index);
        p.hle_.Dispatch(index, c);
        rec.result = c.gpr[3];
        p.LeaveCall(rec, call_lr);
    }
    static void SC(P& p, C& c, Op) {
        CallRecord& rec = p.calls_[p.call_count_++ & 15];
        rec = {c.cia, true, static_cast<u32>(c.gpr[11]), {c.gpr[3], c.gpr[4], c.gpr[5], c.gpr[6]}, 0};
        const u32 call_lr = static_cast<u32>(c.lr);
        p.EnterCall(c, PpuLiveStatus::kInSyscall, static_cast<u32>(c.gpr[11]));
        p.syscalls_.Dispatch(c);
        rec.result = c.gpr[3];
        p.LeaveCall(rec, call_lr);
    }

    // =========================== branches ===========================
    static void B(P& p, C& c, Op op) {
        const u32 target = static_cast<u32>((op.aa() ? 0 : c.cia) + op.li());
        if (op.lk()) c.lr = c.cia + 4;
        p.nia_ = target;
    }
    static void BC(P& p, C& c, Op op) {
        const bool taken = BranchTaken(c, op.bo(), op.bi());
        if (op.lk()) c.lr = c.cia + 4;
        if (taken) p.nia_ = static_cast<u32>((op.aa() ? 0 : c.cia) + op.bd());
    }
    static void BCLR(P& p, C& c, Op op) {
        const u32 target = static_cast<u32>(c.lr) & ~3u;  // read before LK updates LR
        const bool taken = BranchTaken(c, op.bo(), op.bi());
        if (op.lk()) c.lr = c.cia + 4;
        if (taken) p.nia_ = target;
    }
    static void BCCTR(P& p, C& c, Op op) {
        // BO[2] must be set (no CTR decrement); condition only.
        const bool taken = (op.bo() & 0x10) || (CrBit(c, op.bi()) == ((op.bo() >> 3) & 1));
        if (op.lk()) c.lr = c.cia + 4;
        if (taken) p.nia_ = static_cast<u32>(c.ctr) & ~3u;
    }

    // =========================== condition register ===========================
    static void MCRF(P&, C& c, Op op) { SetCrField(c, op.crfd(), (c.cr >> (28 - 4 * op.crfs())) & 0xF); }
#define PPU_CR_LOGIC(NAME, EXPR)                                         \
    static void NAME(P&, C& c, Op op) {                                  \
        const u32 a = CrBit(c, op.ra()), b = CrBit(c, op.rb());          \
        SetCrBit(c, op.rd(), (EXPR) & 1);                                \
    }
    PPU_CR_LOGIC(CRAND, a & b)
    PPU_CR_LOGIC(CROR, a | b)
    PPU_CR_LOGIC(CRXOR, a ^ b)
    PPU_CR_LOGIC(CRNAND, ~(a & b))
    PPU_CR_LOGIC(CRNOR, ~(a | b))
    PPU_CR_LOGIC(CREQV, ~(a ^ b))
    PPU_CR_LOGIC(CRANDC, a & ~b)
    PPU_CR_LOGIC(CRORC, a | ~b)
#undef PPU_CR_LOGIC
    static void MFCR(P&, C& c, Op op) { c.gpr[op.rd()] = c.cr; }  // mfocrf: other fields are undefined
    static void MTCRF(P&, C& c, Op op) {
        u32 mask = 0;
        for (u32 i = 0; i < 8; ++i)
            if (op.crm() & (0x80u >> i)) mask |= 0xF0000000u >> (4 * i);
        c.cr = (c.cr & ~mask) | (static_cast<u32>(c.gpr[op.rd()]) & mask);
    }

    // =========================== sync / cache / SPR ===========================
    static void ISYNC(P&, C&, Op) {}
    // sync (L=0): full barrier, orders store->load (x86 needs MFENCE for that).
    // lwsync (L=1) and eieio: x86-TSO already orders everything else; acq_rel keeps the
    // compiler from moving our own memory accesses across them.
    static void SYNC(P&, C&, Op op) {
        if (((op.raw >> 21) & 3) == 1) std::atomic_thread_fence(std::memory_order_acq_rel);
        else std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    static void EIEIO(P&, C&, Op) { std::atomic_thread_fence(std::memory_order_acq_rel); }
    static void DCBST(P&, C&, Op) {}
    static void DCBF(P&, C&, Op) {}
    static void DCBT(P&, C&, Op) {}
    static void DCBTST(P&, C&, Op) {}
    static void DST(P&, C&, Op) {}
    static void DSTST(P&, C&, Op) {}
    static void DSS(P&, C&, Op) {}
    static void ICBI(P& p, C& c, Op op) { p.InvalidateCode(EaX(c, op) & ~(kCacheLine - 1), kCacheLine); }
    static void DCBZ(P& p, C& c, Op op) {
        static constexpr u8 kZero[kCacheLine] = {};
        p.WriteBytes(c, EaX(c, op) & ~(kCacheLine - 1), kZero, kCacheLine);
    }
    static void MFSPR(P& p, C& c, Op op) {
        u64& rt = c.gpr[op.rd()];
        switch (op.spr()) {
        case 1: rt = c.xer; break;
        case 8: rt = c.lr; break;
        case 9: rt = c.ctr; break;
        case 256: rt = c.vrsave; break;
        case 268: rt = P::TimeBase(); break;
        case 269: rt = P::TimeBase() >> 32; break;
        default: {
            char text[48];
            std::snprintf(text, sizeof(text), "mfspr SPR %u", op.spr());
            p.Stop(c, StopReason::Privileged, 0, false, text);
        }
        }
    }
    static void MTSPR(P& p, C& c, Op op) {
        const u64 rs = c.gpr[op.rd()];
        switch (op.spr()) {
        case 1: c.xer = rs & (kXerSO | kXerOV | kXerCA | 0x7F); break;
        case 8: c.lr = rs; break;
        case 9: c.ctr = rs; break;
        case 256: c.vrsave = static_cast<u32>(rs); break;
        default: {
            char text[48];
            std::snprintf(text, sizeof(text), "mtspr SPR %u", op.spr());
            p.Stop(c, StopReason::Privileged, 0, false, text);
        }
        }
    }
    static void MFTB(P&, C& c, Op op) {
        const u64 tb = P::TimeBase();
        c.gpr[op.rd()] = op.spr() == 269 ? (tb >> 32) : tb;
    }

    // =========================== traps ===========================
    static void TWI(P& p, C& c, Op op) {
        const s32 a = static_cast<s32>(c.gpr[op.ra()]), b = static_cast<s32>(op.simm16());
        if (TrapCond(op.rd(), a, b, static_cast<u32>(a), static_cast<u32>(b))) DoTrap(p, c);
    }
    static void TW(P& p, C& c, Op op) {
        const s32 a = static_cast<s32>(c.gpr[op.ra()]), b = static_cast<s32>(c.gpr[op.rb()]);
        if (TrapCond(op.rd(), a, b, static_cast<u32>(a), static_cast<u32>(b))) DoTrap(p, c);
    }
    static void TDI(P& p, C& c, Op op) {
        const s64 a = static_cast<s64>(c.gpr[op.ra()]), b = op.simm16();
        if (TrapCond(op.rd(), a, b, static_cast<u64>(a), static_cast<u64>(b))) DoTrap(p, c);
    }
    static void TD(P& p, C& c, Op op) {
        const s64 a = static_cast<s64>(c.gpr[op.ra()]), b = static_cast<s64>(c.gpr[op.rb()]);
        if (TrapCond(op.rd(), a, b, static_cast<u64>(a), static_cast<u64>(b))) DoTrap(p, c);
    }

    // =========================== integer arithmetic ===========================
    static void ADDI(P&, C& c, Op op) { c.gpr[op.rd()] = RA0(c, op) + op.simm16(); }
    static void ADDIS(P&, C& c, Op op) { c.gpr[op.rd()] = RA0(c, op) + static_cast<u64>(op.simm16() * 65536); }
    static void ADDIC(P&, C& c, Op op) {
        bool ca;
        c.gpr[op.rd()] = AddCarry(c.gpr[op.ra()], static_cast<u64>(op.simm16()), false, ca);
        SetCa(c, ca);
    }
    static void ADDIC_(P& p, C& c, Op op) {
        ADDIC(p, c, op);
        Cr0(c, c.gpr[op.rd()]);
    }
    static void SUBFIC(P&, C& c, Op op) {  // RT = ~RA + EXTS(SI) + 1
        bool ca;
        c.gpr[op.rd()] = AddCarry(~c.gpr[op.ra()], static_cast<u64>(op.simm16()), true, ca);
        SetCa(c, ca);
    }
    static void MULLI(P&, C& c, Op op) {
        c.gpr[op.rd()] = static_cast<u64>(static_cast<s64>(c.gpr[op.ra()]) * op.simm16());
    }

    // XO-form add/subtract family: RT = a + b + cin, with optional CA, OV (OE) and CR0 (Rc).
    static void AddFamily(C& c, Op op, u64 a, u64 b, bool cin, bool set_ca) {
        bool ca;
        const u64 r = AddCarry(a, b, cin, ca);
        if (set_ca) SetCa(c, ca);
        if (op.oe()) SetOv(c, AddOverflow(a, b, r));
        c.gpr[op.rd()] = r;
        if (op.rc()) Cr0(c, r);
    }
    static void ADD(P&, C& c, Op op) { AddFamily(c, op, c.gpr[op.ra()], c.gpr[op.rb()], false, false); }
    static void ADDC(P&, C& c, Op op) { AddFamily(c, op, c.gpr[op.ra()], c.gpr[op.rb()], false, true); }
    static void ADDE(P&, C& c, Op op) { AddFamily(c, op, c.gpr[op.ra()], c.gpr[op.rb()], Ca(c), true); }
    static void ADDZE(P&, C& c, Op op) { AddFamily(c, op, c.gpr[op.ra()], 0, Ca(c), true); }
    static void ADDME(P&, C& c, Op op) { AddFamily(c, op, c.gpr[op.ra()], ~u64{0}, Ca(c), true); }
    static void SUBF(P&, C& c, Op op) { AddFamily(c, op, ~c.gpr[op.ra()], c.gpr[op.rb()], true, false); }
    static void SUBFC(P&, C& c, Op op) { AddFamily(c, op, ~c.gpr[op.ra()], c.gpr[op.rb()], true, true); }
    static void SUBFE(P&, C& c, Op op) { AddFamily(c, op, ~c.gpr[op.ra()], c.gpr[op.rb()], Ca(c), true); }
    static void SUBFZE(P&, C& c, Op op) { AddFamily(c, op, ~c.gpr[op.ra()], 0, Ca(c), true); }
    static void SUBFME(P&, C& c, Op op) { AddFamily(c, op, ~c.gpr[op.ra()], ~u64{0}, Ca(c), true); }
    static void NEG(P&, C& c, Op op) {
        const u64 a = c.gpr[op.ra()];
        const u64 r = ~a + 1;
        if (op.oe()) SetOv(c, a == 0x8000000000000000ull);
        c.gpr[op.rd()] = r;
        if (op.rc()) Cr0(c, r);
    }

    static void MULLW(P&, C& c, Op op) {
        const s64 r = s64{static_cast<s32>(c.gpr[op.ra()])} * static_cast<s32>(c.gpr[op.rb()]);
        if (op.oe()) SetOv(c, r != static_cast<s32>(r));
        c.gpr[op.rd()] = static_cast<u64>(r);
        if (op.rc()) Cr0(c, c.gpr[op.rd()]);
    }
    static void MULHW(P&, C& c, Op op) {
        const s64 r = (s64{static_cast<s32>(c.gpr[op.ra()])} * static_cast<s32>(c.gpr[op.rb()])) >> 32;
        c.gpr[op.rd()] = static_cast<u64>(r);  // high word, sign-extended (upper half undefined)
        if (op.rc()) Cr0(c, c.gpr[op.rd()]);
    }
    static void MULHWU(P&, C& c, Op op) {
        c.gpr[op.rd()] = (u64{static_cast<u32>(c.gpr[op.ra()])} * static_cast<u32>(c.gpr[op.rb()])) >> 32;
        if (op.rc()) Cr0(c, c.gpr[op.rd()]);
    }
    static void MULLD(P&, C& c, Op op) {
        const s64 a = static_cast<s64>(c.gpr[op.ra()]), b = static_cast<s64>(c.gpr[op.rb()]);
        const u64 r = static_cast<u64>(a) * static_cast<u64>(b);
        if (op.oe()) SetOv(c, MulHighS64(a, b) != (static_cast<s64>(r) >> 63));
        c.gpr[op.rd()] = r;
        if (op.rc()) Cr0(c, r);
    }
    static void MULHD(P&, C& c, Op op) {
        c.gpr[op.rd()] = static_cast<u64>(MulHighS64(static_cast<s64>(c.gpr[op.ra()]), static_cast<s64>(c.gpr[op.rb()])));
        if (op.rc()) Cr0(c, c.gpr[op.rd()]);
    }
    static void MULHDU(P&, C& c, Op op) {
        c.gpr[op.rd()] = MulHighU64(c.gpr[op.ra()], c.gpr[op.rb()]);
        if (op.rc()) Cr0(c, c.gpr[op.rd()]);
    }
    // Division by zero / overflow: result undefined on hardware; 0 here, OV set if OE.
    static void DIVW(P&, C& c, Op op) {
        const s32 a = static_cast<s32>(c.gpr[op.ra()]), b = static_cast<s32>(c.gpr[op.rb()]);
        const bool ov = b == 0 || (a == std::numeric_limits<s32>::min() && b == -1);
        c.gpr[op.rd()] = ov ? 0 : static_cast<u32>(a / b);
        if (op.oe()) SetOv(c, ov);
        if (op.rc()) Cr0(c, c.gpr[op.rd()]);
    }
    static void DIVWU(P&, C& c, Op op) {
        const u32 a = static_cast<u32>(c.gpr[op.ra()]), b = static_cast<u32>(c.gpr[op.rb()]);
        c.gpr[op.rd()] = b ? a / b : 0;
        if (op.oe()) SetOv(c, b == 0);
        if (op.rc()) Cr0(c, c.gpr[op.rd()]);
    }
    static void DIVD(P&, C& c, Op op) {
        const s64 a = static_cast<s64>(c.gpr[op.ra()]), b = static_cast<s64>(c.gpr[op.rb()]);
        const bool ov = b == 0 || (a == std::numeric_limits<s64>::min() && b == -1);
        c.gpr[op.rd()] = ov ? 0 : static_cast<u64>(a / b);
        if (op.oe()) SetOv(c, ov);
        if (op.rc()) Cr0(c, c.gpr[op.rd()]);
    }
    static void DIVDU(P&, C& c, Op op) {
        const u64 a = c.gpr[op.ra()], b = c.gpr[op.rb()];
        c.gpr[op.rd()] = b ? a / b : 0;
        if (op.oe()) SetOv(c, b == 0);
        if (op.rc()) Cr0(c, c.gpr[op.rd()]);
    }

    // =========================== compares ===========================
    static void CMPI(P&, C& c, Op op) {
        if (op.l10()) Compare<s64>(c, op.crfd(), static_cast<s64>(c.gpr[op.ra()]), op.simm16());
        else Compare<s32>(c, op.crfd(), static_cast<s32>(c.gpr[op.ra()]), static_cast<s32>(op.simm16()));
    }
    static void CMPLI(P&, C& c, Op op) {
        if (op.l10()) Compare<u64>(c, op.crfd(), c.gpr[op.ra()], op.uimm16());
        else Compare<u32>(c, op.crfd(), static_cast<u32>(c.gpr[op.ra()]), static_cast<u32>(op.uimm16()));
    }
    static void CMP(P&, C& c, Op op) {
        if (op.l10()) Compare<s64>(c, op.crfd(), static_cast<s64>(c.gpr[op.ra()]), static_cast<s64>(c.gpr[op.rb()]));
        else Compare<s32>(c, op.crfd(), static_cast<s32>(c.gpr[op.ra()]), static_cast<s32>(c.gpr[op.rb()]));
    }
    static void CMPL(P&, C& c, Op op) {
        if (op.l10()) Compare<u64>(c, op.crfd(), c.gpr[op.ra()], c.gpr[op.rb()]);
        else Compare<u32>(c, op.crfd(), static_cast<u32>(c.gpr[op.ra()]), static_cast<u32>(c.gpr[op.rb()]));
    }

    // =========================== logical (dest RA, source RS) ===========================
    static void ORI(P&, C& c, Op op) { c.gpr[op.ra()] = c.gpr[op.rd()] | op.uimm16(); }
    static void ORIS(P&, C& c, Op op) { c.gpr[op.ra()] = c.gpr[op.rd()] | (op.uimm16() << 16); }
    static void XORI(P&, C& c, Op op) { c.gpr[op.ra()] = c.gpr[op.rd()] ^ op.uimm16(); }
    static void XORIS(P&, C& c, Op op) { c.gpr[op.ra()] = c.gpr[op.rd()] ^ (op.uimm16() << 16); }
    static void ANDI_(P&, C& c, Op op) {
        c.gpr[op.ra()] = c.gpr[op.rd()] & op.uimm16();
        Cr0(c, c.gpr[op.ra()]);
    }
    static void ANDIS_(P&, C& c, Op op) {
        c.gpr[op.ra()] = c.gpr[op.rd()] & (op.uimm16() << 16);
        Cr0(c, c.gpr[op.ra()]);
    }
#define PPU_LOGIC(NAME, EXPR)                         \
    static void NAME(P&, C& c, Op op) {               \
        const u64 s = c.gpr[op.rd()], b = c.gpr[op.rb()]; \
        (void)b;                                      \
        c.gpr[op.ra()] = (EXPR);                      \
        if (op.rc()) Cr0(c, c.gpr[op.ra()]);          \
    }
    PPU_LOGIC(AND, s & b)
    PPU_LOGIC(OR, s | b)
    PPU_LOGIC(XOR, s ^ b)
    PPU_LOGIC(NAND, ~(s & b))
    PPU_LOGIC(NOR, ~(s | b))
    PPU_LOGIC(EQV, ~(s ^ b))
    PPU_LOGIC(ANDC, s & ~b)
    PPU_LOGIC(ORC, s | ~b)
    PPU_LOGIC(EXTSB, static_cast<u64>(static_cast<s64>(static_cast<s8>(s))))
    PPU_LOGIC(EXTSH, static_cast<u64>(static_cast<s64>(static_cast<s16>(s))))
    PPU_LOGIC(EXTSW, static_cast<u64>(static_cast<s64>(static_cast<s32>(s))))
    PPU_LOGIC(CNTLZW, static_cast<u64>(std::countl_zero(static_cast<u32>(s))))
    PPU_LOGIC(CNTLZD, static_cast<u64>(std::countl_zero(s)))
    // Shifts: amount from RB (6 bits for word, 7 for doubleword); out of range gives 0.
    PPU_LOGIC(SLW, (b & 0x20) ? 0 : u64{static_cast<u32>(s << (b & 0x1F))})
    PPU_LOGIC(SRW, (b & 0x20) ? 0 : u64{static_cast<u32>(s) >> (b & 0x1F)})
    PPU_LOGIC(SLD, (b & 0x40) ? 0 : s << (b & 0x3F))
    PPU_LOGIC(SRD, (b & 0x40) ? 0 : s >> (b & 0x3F))
#undef PPU_LOGIC

    // Algebraic shifts set CA when the source is negative and 1-bits were shifted out.
    static void ShiftRightWordAlg(C& c, Op op, u32 n) {
        const s32 v = static_cast<s32>(c.gpr[op.rd()]);
        s32 r;
        bool ca;
        if (n > 31) {
            r = v >> 31;
            ca = v < 0;
        } else {
            r = v >> n;
            ca = v < 0 && n && (static_cast<u32>(v) & ((1u << n) - 1));
        }
        c.gpr[op.ra()] = static_cast<u64>(static_cast<s64>(r));
        SetCa(c, ca);
        if (op.rc()) Cr0(c, c.gpr[op.ra()]);
    }
    static void SRAWI(P&, C& c, Op op) { ShiftRightWordAlg(c, op, op.sh32()); }
    static void SRAW(P&, C& c, Op op) { ShiftRightWordAlg(c, op, static_cast<u32>(c.gpr[op.rb()] & 0x3F)); }
    static void ShiftRightDwordAlg(C& c, Op op, u32 n) {
        const s64 v = static_cast<s64>(c.gpr[op.rd()]);
        s64 r;
        bool ca;
        if (n > 63) {
            r = v >> 63;
            ca = v < 0;
        } else {
            r = v >> n;
            ca = v < 0 && n && (static_cast<u64>(v) & ((u64{1} << n) - 1));
        }
        c.gpr[op.ra()] = static_cast<u64>(r);
        SetCa(c, ca);
        if (op.rc()) Cr0(c, c.gpr[op.ra()]);
    }
    static void SRADI(P&, C& c, Op op) { ShiftRightDwordAlg(c, op, op.sh64()); }
    static void SRAD(P&, C& c, Op op) { ShiftRightDwordAlg(c, op, static_cast<u32>(c.gpr[op.rb()] & 0x7F)); }

    // =========================== rotates ===========================
    static void RLWINM(P&, C& c, Op op) {
        c.gpr[op.ra()] = Rotl32Dup(c.gpr[op.rd()], op.sh32()) & Mask64(op.mb32() + 32, op.me32() + 32);
        if (op.rc()) Cr0(c, c.gpr[op.ra()]);
    }
    static void RLWNM(P&, C& c, Op op) {
        c.gpr[op.ra()] = Rotl32Dup(c.gpr[op.rd()], static_cast<u32>(c.gpr[op.rb()] & 31)) &
                         Mask64(op.mb32() + 32, op.me32() + 32);
        if (op.rc()) Cr0(c, c.gpr[op.ra()]);
    }
    static void RLWIMI(P&, C& c, Op op) {
        const u64 m = Mask64(op.mb32() + 32, op.me32() + 32);
        c.gpr[op.ra()] = (Rotl32Dup(c.gpr[op.rd()], op.sh32()) & m) | (c.gpr[op.ra()] & ~m);
        if (op.rc()) Cr0(c, c.gpr[op.ra()]);
    }
    static void RLDICL(P&, C& c, Op op) {
        c.gpr[op.ra()] = Rotl64(c.gpr[op.rd()], op.sh64()) & Mask64(op.mbe64(), 63);
        if (op.rc()) Cr0(c, c.gpr[op.ra()]);
    }
    static void RLDICR(P&, C& c, Op op) {
        c.gpr[op.ra()] = Rotl64(c.gpr[op.rd()], op.sh64()) & Mask64(0, op.mbe64());
        if (op.rc()) Cr0(c, c.gpr[op.ra()]);
    }
    static void RLDIC(P&, C& c, Op op) {
        c.gpr[op.ra()] = Rotl64(c.gpr[op.rd()], op.sh64()) & Mask64(op.mbe64(), 63 - op.sh64());
        if (op.rc()) Cr0(c, c.gpr[op.ra()]);
    }
    static void RLDIMI(P&, C& c, Op op) {
        const u64 m = Mask64(op.mbe64(), 63 - op.sh64());
        c.gpr[op.ra()] = (Rotl64(c.gpr[op.rd()], op.sh64()) & m) | (c.gpr[op.ra()] & ~m);
        if (op.rc()) Cr0(c, c.gpr[op.ra()]);
    }
    static void RLDCL(P&, C& c, Op op) {
        c.gpr[op.ra()] = Rotl64(c.gpr[op.rd()], static_cast<u32>(c.gpr[op.rb()] & 63)) & Mask64(op.mbe64(), 63);
        if (op.rc()) Cr0(c, c.gpr[op.ra()]);
    }
    static void RLDCR(P&, C& c, Op op) {
        c.gpr[op.ra()] = Rotl64(c.gpr[op.rd()], static_cast<u32>(c.gpr[op.rb()] & 63)) & Mask64(0, op.mbe64());
        if (op.rc()) Cr0(c, c.gpr[op.ra()]);
    }

    // =========================== integer loads ===========================
    // Update forms write the EA back to RA only if the access succeeded.
#define PPU_LOAD(NAME, T, SIGNED, EA)                                  \
    static void NAME(P& p, C& c, Op op) { LoadGpr<T, SIGNED>(p, c, op.rd(), EA(c, op)); }
#define PPU_LOAD_U(NAME, T, SIGNED, EA)                                \
    static void NAME(P& p, C& c, Op op) {                              \
        const u32 ea = EA(c, op);                                      \
        if (LoadGpr<T, SIGNED>(p, c, op.rd(), ea)) c.gpr[op.ra()] = ea;\
    }
    PPU_LOAD(LBZ, u8, false, EaD)
    PPU_LOAD(LHZ, u16, false, EaD)
    PPU_LOAD(LHA, u16, true, EaD)
    PPU_LOAD(LWZ, u32, false, EaD)
    PPU_LOAD(LWA, u32, true, EaDS)
    PPU_LOAD(LD, u64, false, EaDS)
    PPU_LOAD(LBZX, u8, false, EaX)
    PPU_LOAD(LHZX, u16, false, EaX)
    PPU_LOAD(LHAX, u16, true, EaX)
    PPU_LOAD(LWZX, u32, false, EaX)
    PPU_LOAD(LWAX, u32, true, EaX)
    PPU_LOAD(LDX, u64, false, EaX)
    PPU_LOAD_U(LBZU, u8, false, EaDU)
    PPU_LOAD_U(LHZU, u16, false, EaDU)
    PPU_LOAD_U(LHAU, u16, true, EaDU)
    PPU_LOAD_U(LWZU, u32, false, EaDU)
    PPU_LOAD_U(LDU, u64, false, EaDSU)
    PPU_LOAD_U(LBZUX, u8, false, EaXU)
    PPU_LOAD_U(LHZUX, u16, false, EaXU)
    PPU_LOAD_U(LHAUX, u16, true, EaXU)
    PPU_LOAD_U(LWZUX, u32, false, EaXU)
    PPU_LOAD_U(LWAUX, u32, true, EaXU)
    PPU_LOAD_U(LDUX, u64, false, EaXU)
#undef PPU_LOAD
#undef PPU_LOAD_U

#define PPU_STORE(NAME, T, EA) \
    static void NAME(P& p, C& c, Op op) { StoreGpr<T>(p, c, op.rd(), EA(c, op)); }
#define PPU_STORE_U(NAME, T, EA)                                  \
    static void NAME(P& p, C& c, Op op) {                         \
        const u32 ea = EA(c, op);                                 \
        if (StoreGpr<T>(p, c, op.rd(), ea)) c.gpr[op.ra()] = ea;  \
    }
    PPU_STORE(STB, u8, EaD)
    PPU_STORE(STH, u16, EaD)
    PPU_STORE(STW, u32, EaD)
    PPU_STORE(STD, u64, EaDS)
    PPU_STORE(STBX, u8, EaX)
    PPU_STORE(STHX, u16, EaX)
    PPU_STORE(STWX, u32, EaX)
    PPU_STORE(STDX, u64, EaX)
    PPU_STORE_U(STBU, u8, EaDU)
    PPU_STORE_U(STHU, u16, EaDU)
    PPU_STORE_U(STWU, u32, EaDU)
    PPU_STORE_U(STDU, u64, EaDSU)
    PPU_STORE_U(STBUX, u8, EaXU)
    PPU_STORE_U(STHUX, u16, EaXU)
    PPU_STORE_U(STWUX, u32, EaXU)
    PPU_STORE_U(STDUX, u64, EaXU)
#undef PPU_STORE
#undef PPU_STORE_U

    // Byte-reversed: the guest asks for little-endian, i.e. the host-native byte order.
    static void LWBRX(P& p, C& c, Op op) {
        u32 v = 0;
        if (p.Read(c, EaX(c, op), v)) c.gpr[op.rd()] = ByteSwap(v);
    }
    static void LHBRX(P& p, C& c, Op op) {
        u16 v = 0;
        if (p.Read(c, EaX(c, op), v)) c.gpr[op.rd()] = ByteSwap(v);
    }
    static void STWBRX(P& p, C& c, Op op) { p.Write(c, EaX(c, op), ByteSwap(static_cast<u32>(c.gpr[op.rd()]))); }
    static void STHBRX(P& p, C& c, Op op) { p.Write(c, EaX(c, op), ByteSwap(static_cast<u16>(c.gpr[op.rd()]))); }

    static void LMW(P& p, C& c, Op op) {
        u32 ea = EaD(c, op);
        for (u32 r = op.rd(); r < 32; ++r, ea += 4)
            if (!LoadGpr<u32>(p, c, r, ea)) return;
    }
    static void STMW(P& p, C& c, Op op) {
        u32 ea = EaD(c, op);
        for (u32 r = op.rd(); r < 32; ++r, ea += 4)
            if (!StoreGpr<u32>(p, c, r, ea)) return;
    }

    // =========================== atomics ===========================
    // Reservation = (address, value seen). The conditional store succeeds only if memory still
    // holds that value, done with a host CAS so it stays correct once several PPU threads run.
    template <typename T>
    static void LoadReserve(P& p, C& c, Op op) {
        const u32 ea = EaX(c, op);
        if (ea & (sizeof(T) - 1)) return p.Stop(c, StopReason::Alignment, ea, false, "misaligned load-reserve");
        T value{};
        if (!p.Read(c, ea, value)) return;
        p.reserved_ = true;
        p.reserve_addr_ = ea;
        p.reserve_size_ = sizeof(T);
        p.reserve_raw_ = ByteSwap(value);  // raw big-endian pattern as stored in memory
        c.gpr[op.rd()] = value;
    }
    template <typename T>
    static void StoreConditional(P& p, C& c, Op op) {
        const u32 ea = EaX(c, op);
        if (ea & (sizeof(T) - 1)) return p.Stop(c, StopReason::Alignment, ea, true, "misaligned store-conditional");
        if (!p.Accessible(ea, sizeof(T), vm::kPageWritable) && !p.RecoverFault(c, ea, sizeof(T), vm::kPageWritable, true))
            return p.Stop(c, StopReason::DataFault, ea, true);
        bool success = false;
        if (p.reserved_ && p.reserve_addr_ == ea && p.reserve_size_ == sizeof(T)) {
            std::atomic_ref<T> cell(*reinterpret_cast<T*>(p.base_ + ea));
            T expected = static_cast<T>(p.reserve_raw_);
            success = cell.compare_exchange_strong(expected, ByteSwap(static_cast<T>(c.gpr[op.rd()])));
            if (success) p.NoteCodeWrite(ea, sizeof(T));
        }
        p.reserved_ = false;
        SetCrField(c, 0, (success ? kCrEQ : 0) | So(c));
    }
    static void LWARX(P& p, C& c, Op op) { LoadReserve<u32>(p, c, op); }
    static void LDARX(P& p, C& c, Op op) { LoadReserve<u64>(p, c, op); }
    static void STWCX(P& p, C& c, Op op) { StoreConditional<u32>(p, c, op); }
    static void STDCX(P& p, C& c, Op op) { StoreConditional<u64>(p, c, op); }

    // =========================== FPU loads/stores ===========================
    static void LFS(P& p, C& c, Op op) { LoadF32(p, c, op.rd(), EaD(c, op)); }
    static void LFD(P& p, C& c, Op op) { LoadF64(p, c, op.rd(), EaD(c, op)); }
    static void LFSX(P& p, C& c, Op op) { LoadF32(p, c, op.rd(), EaX(c, op)); }
    static void LFDX(P& p, C& c, Op op) { LoadF64(p, c, op.rd(), EaX(c, op)); }
    static void STFS(P& p, C& c, Op op) { StoreF32(p, c, op.rd(), EaD(c, op)); }
    static void STFD(P& p, C& c, Op op) { StoreF64(p, c, op.rd(), EaD(c, op)); }
    static void STFSX(P& p, C& c, Op op) { StoreF32(p, c, op.rd(), EaX(c, op)); }
    static void STFDX(P& p, C& c, Op op) { StoreF64(p, c, op.rd(), EaX(c, op)); }
#define PPU_FP_U(NAME, FN, EA)                               \
    static void NAME(P& p, C& c, Op op) {                    \
        const u32 ea = EA(c, op);                            \
        if (FN(p, c, op.rd(), ea)) c.gpr[op.ra()] = ea;      \
    }
    PPU_FP_U(LFSU, LoadF32, EaDU)
    PPU_FP_U(LFDU, LoadF64, EaDU)
    PPU_FP_U(LFSUX, LoadF32, EaXU)
    PPU_FP_U(LFDUX, LoadF64, EaXU)
    PPU_FP_U(STFSU, StoreF32, EaDU)
    PPU_FP_U(STFDU, StoreF64, EaDU)
    PPU_FP_U(STFSUX, StoreF32, EaXU)
    PPU_FP_U(STFDUX, StoreF64, EaXU)
#undef PPU_FP_U
    static void STFIWX(P& p, C& c, Op op) {
        p.Write(c, EaX(c, op), static_cast<u32>(DoubleBits(c.fpr[op.rd()])));
    }

    // =========================== FPU arithmetic ===========================
    // Round-to-nearest assumed (FPSCR[RN] not honoured yet). Single-precision forms compute
    // in double and round once to float.
    static f64 Single(f64 v) noexcept { return static_cast<f64>(static_cast<f32>(v)); }
#define PPU_FP(NAME, EXPR, ROUND)                                                       \
    static void NAME(P&, C& c, Op op) {                                                 \
        const f64 a = c.fpr[op.ra()], b = c.fpr[op.rb()], fc = c.fpr[op.frc()];        \
        (void)a; (void)b; (void)fc;                                                     \
        c.fpr[op.rd()] = ROUND(EXPR);                                                   \
        FpRc(c, op);                                                                    \
    }
#define PPU_NO_ROUND(x) (x)
    PPU_FP(FADD, a + b, PPU_NO_ROUND)
    PPU_FP(FSUB, a - b, PPU_NO_ROUND)
    PPU_FP(FMUL, a * fc, PPU_NO_ROUND)
    PPU_FP(FDIV, a / b, PPU_NO_ROUND)
    PPU_FP(FSQRT, std::sqrt(b), PPU_NO_ROUND)
    PPU_FP(FMADD, std::fma(a, fc, b), PPU_NO_ROUND)
    PPU_FP(FMSUB, std::fma(a, fc, -b), PPU_NO_ROUND)
    PPU_FP(FNMADD, -std::fma(a, fc, b), PPU_NO_ROUND)
    PPU_FP(FNMSUB, -std::fma(a, fc, -b), PPU_NO_ROUND)
    PPU_FP(FSEL, a >= 0.0 ? fc : b, PPU_NO_ROUND)
    PPU_FP(FRSQRTE, 1.0 / std::sqrt(b), PPU_NO_ROUND)
    PPU_FP(FADDS, a + b, Single)
    PPU_FP(FSUBS, a - b, Single)
    PPU_FP(FMULS, a * fc, Single)
    PPU_FP(FDIVS, a / b, Single)
    PPU_FP(FSQRTS, std::sqrt(b), Single)
    PPU_FP(FRES, 1.0 / b, Single)
    PPU_FP(FMADDS, std::fma(a, fc, b), Single)
    PPU_FP(FMSUBS, std::fma(a, fc, -b), Single)
    PPU_FP(FNMADDS, -std::fma(a, fc, b), Single)
    PPU_FP(FNMSUBS, -std::fma(a, fc, -b), Single)
    PPU_FP(FMR, b, PPU_NO_ROUND)
    PPU_FP(FNEG, -b, PPU_NO_ROUND)
    PPU_FP(FABS, std::fabs(b), PPU_NO_ROUND)
    PPU_FP(FNABS, -std::fabs(b), PPU_NO_ROUND)
    PPU_FP(FRSP, b, Single)
    PPU_FP(FCFID, static_cast<f64>(static_cast<s64>(DoubleBits(b))), PPU_NO_ROUND)
#undef PPU_FP
#undef PPU_NO_ROUND

    static u64 ToS32(f64 v, bool truncate) noexcept {
        if (std::isnan(v)) return 0x80000000u;
        const f64 r = truncate ? std::trunc(v) : std::nearbyint(v);
        if (r >= 2147483647.0) return 0x7FFFFFFFu;
        if (r <= -2147483648.0) return 0x80000000u;
        return static_cast<u32>(static_cast<s32>(r));
    }
    static u64 ToS64(f64 v, bool truncate) noexcept {
        if (std::isnan(v)) return 0x8000000000000000ull;
        const f64 r = truncate ? std::trunc(v) : std::nearbyint(v);
        if (r >= 9223372036854775807.0) return 0x7FFFFFFFFFFFFFFFull;
        if (r <= -9223372036854775808.0) return 0x8000000000000000ull;
        return static_cast<u64>(static_cast<s64>(r));
    }
    static void FCTIW(P&, C& c, Op op) { c.fpr[op.rd()] = BitsDouble(ToS32(c.fpr[op.rb()], false)); FpRc(c, op); }
    static void FCTIWZ(P&, C& c, Op op) { c.fpr[op.rd()] = BitsDouble(ToS32(c.fpr[op.rb()], true)); FpRc(c, op); }
    static void FCTID(P&, C& c, Op op) { c.fpr[op.rd()] = BitsDouble(ToS64(c.fpr[op.rb()], false)); FpRc(c, op); }
    static void FCTIDZ(P&, C& c, Op op) { c.fpr[op.rd()] = BitsDouble(ToS64(c.fpr[op.rb()], true)); FpRc(c, op); }

    static void FCMPU(P&, C& c, Op op) {
        const f64 a = c.fpr[op.ra()], b = c.fpr[op.rb()];
        const u32 r = (std::isnan(a) || std::isnan(b)) ? 1u : a < b ? kCrLT : a > b ? kCrGT : kCrEQ;
        SetCrField(c, op.crfd(), r);
        c.fpscr = (c.fpscr & ~0xF000u) | (r << 12);  // FPCC
    }
    static void FCMPO(P& p, C& c, Op op) { FCMPU(p, c, op); }
    static void MFFS(P&, C& c, Op op) {
        c.fpr[op.rd()] = BitsDouble(c.fpscr);
        FpRc(c, op);
    }
    static void MTFSF(P&, C& c, Op op) {
        u32 mask = 0;
        for (u32 i = 0; i < 8; ++i)
            if (op.flm() & (0x80u >> i)) mask |= 0xF0000000u >> (4 * i);
        c.fpscr = (c.fpscr & ~mask) | (static_cast<u32>(DoubleBits(c.fpr[op.rb()])) & mask);
        FpRc(c, op);
    }
    static void MTFSFI(P&, C& c, Op op) {
        const u32 shift = 28 - 4 * op.crfd();
        c.fpscr = (c.fpscr & ~(0xFu << shift)) | (op.fpimm() << shift);
        FpRc(c, op);
    }
    static void MTFSB0(P&, C& c, Op op) { c.fpscr &= ~(0x80000000u >> op.rd()); FpRc(c, op); }
    static void MTFSB1(P&, C& c, Op op) { c.fpscr |= 0x80000000u >> op.rd(); FpRc(c, op); }
    static void MCRFS(P&, C& c, Op op) { SetCrField(c, op.crfd(), c.fpscr >> (28 - 4 * op.crfs())); }

    // =========================== VMX memory ===========================
    static void LVX(P& p, C& c, Op op) {
        v128 v;
        if (p.ReadBytes(c, EaX(c, op) & ~15u, v.bytes, 16)) c.vr[op.rd()] = v;  // BE order kept as-is
    }
    static void STVX(P& p, C& c, Op op) { p.WriteBytes(c, EaX(c, op) & ~15u, c.vr[op.rd()].bytes, 16); }
    static void LVSL(P&, C& c, Op op) {
        const u32 sh = EaX(c, op) & 15;
        for (u32 i = 0; i < 16; ++i) c.vr[op.rd()].bytes[i] = static_cast<u8>(sh + i);
    }
    static void LVSR(P&, C& c, Op op) {
        const u32 sh = EaX(c, op) & 15;
        for (u32 i = 0; i < 16; ++i) c.vr[op.rd()].bytes[i] = static_cast<u8>(16 - sh + i);
    }
    template <u32 kSize>
    static void LoadElement(P& p, C& c, Op op) {
        const u32 ea = EaX(c, op) & ~(kSize - 1);
        p.ReadBytes(c, ea, c.vr[op.rd()].bytes + (ea & 15), kSize);
    }
    template <u32 kSize>
    static void StoreElement(P& p, C& c, Op op) {
        const u32 ea = EaX(c, op) & ~(kSize - 1);
        p.WriteBytes(c, ea, c.vr[op.rd()].bytes + (ea & 15), kSize);
    }
    static void LVEBX(P& p, C& c, Op op) { LoadElement<1>(p, c, op); }
    static void LVEHX(P& p, C& c, Op op) { LoadElement<2>(p, c, op); }
    static void LVEWX(P& p, C& c, Op op) { LoadElement<4>(p, c, op); }
    static void STVEBX(P& p, C& c, Op op) { StoreElement<1>(p, c, op); }
    static void STVEHX(P& p, C& c, Op op) { StoreElement<2>(p, c, op); }
    static void STVEWX(P& p, C& c, Op op) { StoreElement<4>(p, c, op); }
    // Cell unaligned quadword pairs: lvlx loads [EA, end of 16-byte block) into the left of
    // the register, lvrx loads [block start, EA) into the right; zero elsewhere.
    static void LVLX(P& p, C& c, Op op) {
        const u32 ea = EaX(c, op), eb = ea & 15;
        v128 v{};
        if (p.ReadBytes(c, ea, v.bytes, 16 - eb)) c.vr[op.rd()] = v;
    }
    static void LVRX(P& p, C& c, Op op) {
        const u32 ea = EaX(c, op), eb = ea & 15;
        v128 v{};
        if (eb && !p.ReadBytes(c, ea & ~15u, v.bytes + 16 - eb, eb)) return;
        c.vr[op.rd()] = v;
    }
    static void STVLX(P& p, C& c, Op op) {
        const u32 ea = EaX(c, op), eb = ea & 15;
        p.WriteBytes(c, ea, c.vr[op.rd()].bytes, 16 - eb);
    }
    static void STVRX(P& p, C& c, Op op) {
        const u32 ea = EaX(c, op), eb = ea & 15;
        if (eb) p.WriteBytes(c, ea & ~15u, c.vr[op.rd()].bytes + 16 - eb, eb);
    }

    // =========================== VMX arithmetic / logic ===========================
    // Operands are copied first so VRT may alias VRA/VRB/VRC.
#define VARGS                                                       \
    const v128 a = c.vr[op.ra()], b = c.vr[op.rb()], vc = c.vr[op.frc()]; \
    (void)a; (void)b; (void)vc;                                     \
    v128 t{}
#define VDONE c.vr[op.rd()] = t
    static void VAND(P&, C& c, Op op) { VARGS; for (int i = 0; i < 16; ++i) t.bytes[i] = a.bytes[i] & b.bytes[i]; VDONE; }
    static void VANDC(P&, C& c, Op op) { VARGS; for (int i = 0; i < 16; ++i) t.bytes[i] = a.bytes[i] & ~b.bytes[i]; VDONE; }
    static void VOR(P&, C& c, Op op) { VARGS; for (int i = 0; i < 16; ++i) t.bytes[i] = a.bytes[i] | b.bytes[i]; VDONE; }
    static void VNOR(P&, C& c, Op op) { VARGS; for (int i = 0; i < 16; ++i) t.bytes[i] = static_cast<u8>(~(a.bytes[i] | b.bytes[i])); VDONE; }
    static void VXOR(P&, C& c, Op op) { VARGS; for (int i = 0; i < 16; ++i) t.bytes[i] = a.bytes[i] ^ b.bytes[i]; VDONE; }
    static void VSEL(P&, C& c, Op op) {
        VARGS;
        for (int i = 0; i < 16; ++i) t.bytes[i] = static_cast<u8>((b.bytes[i] & vc.bytes[i]) | (a.bytes[i] & ~vc.bytes[i]));
        VDONE;
    }
    static void VPERM(P&, C& c, Op op) {
        VARGS;
        for (int i = 0; i < 16; ++i) {
            const u32 index = vc.bytes[i] & 31;
            t.bytes[i] = index < 16 ? a.bytes[index] : b.bytes[index - 16];
        }
        VDONE;
    }
    static void VSLDOI(P&, C& c, Op op) {
        VARGS;
        const u32 sh = op.vshb();
        for (u32 i = 0; i < 16; ++i) t.bytes[i] = (i + sh) < 16 ? a.bytes[i + sh] : b.bytes[i + sh - 16];
        VDONE;
    }
    static void VADDUBM(P&, C& c, Op op) { VARGS; for (int i = 0; i < 16; ++i) t.bytes[i] = static_cast<u8>(a.bytes[i] + b.bytes[i]); VDONE; }
    static void VSUBUBM(P&, C& c, Op op) { VARGS; for (int i = 0; i < 16; ++i) t.bytes[i] = static_cast<u8>(a.bytes[i] - b.bytes[i]); VDONE; }
    static void VADDUHM(P&, C& c, Op op) { VARGS; for (int i = 0; i < 8; ++i) VSetH(t, i, static_cast<u16>(VH(a, i) + VH(b, i))); VDONE; }
    static void VSUBUHM(P&, C& c, Op op) { VARGS; for (int i = 0; i < 8; ++i) VSetH(t, i, static_cast<u16>(VH(a, i) - VH(b, i))); VDONE; }
    static void VADDUWM(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetW(t, i, VW(a, i) + VW(b, i)); VDONE; }
    static void VSUBUWM(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetW(t, i, VW(a, i) - VW(b, i)); VDONE; }
    static void VADDFP(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetF(t, i, VF(a, i) + VF(b, i)); VDONE; }
    static void VSUBFP(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetF(t, i, VF(a, i) - VF(b, i)); VDONE; }
    static void VMADDFP(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetF(t, i, std::fma(VF(a, i), VF(vc, i), VF(b, i))); VDONE; }
    static void VNMSUBFP(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetF(t, i, std::fma(-VF(a, i), VF(vc, i), VF(b, i))); VDONE; }
    static void VMAXFP(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetF(t, i, VF(a, i) > VF(b, i) ? VF(a, i) : VF(b, i)); VDONE; }
    static void VMINFP(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetF(t, i, VF(a, i) < VF(b, i) ? VF(a, i) : VF(b, i)); VDONE; }
    static void VMAXSW(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetW(t, i, static_cast<u32>(std::max(static_cast<s32>(VW(a, i)), static_cast<s32>(VW(b, i))))); VDONE; }
    static void VMINSW(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetW(t, i, static_cast<u32>(std::min(static_cast<s32>(VW(a, i)), static_cast<s32>(VW(b, i))))); VDONE; }
    static void VMAXUW(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetW(t, i, std::max(VW(a, i), VW(b, i))); VDONE; }
    static void VMINUW(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetW(t, i, std::min(VW(a, i), VW(b, i))); VDONE; }
    static void VREFP(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetF(t, i, 1.0f / VF(b, i)); VDONE; }
    static void VRSQRTEFP(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetF(t, i, 1.0f / std::sqrt(VF(b, i))); VDONE; }
    static void VSLW(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetW(t, i, VW(a, i) << (VW(b, i) & 31)); VDONE; }
    static void VSRW(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetW(t, i, VW(a, i) >> (VW(b, i) & 31)); VDONE; }
    static void VSRAW(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetW(t, i, static_cast<u32>(static_cast<s32>(VW(a, i)) >> (VW(b, i) & 31))); VDONE; }
    static void VRLW(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetW(t, i, std::rotl(VW(a, i), static_cast<int>(VW(b, i) & 31))); VDONE; }
    // Splats: element index in the VRA field, source VRB.
    static void VSPLTB(P&, C& c, Op op) { VARGS; for (int i = 0; i < 16; ++i) t.bytes[i] = b.bytes[op.vuimm() & 15]; VDONE; }
    static void VSPLTH(P&, C& c, Op op) { VARGS; for (int i = 0; i < 8; ++i) VSetH(t, i, VH(b, op.vuimm() & 7)); VDONE; }
    static void VSPLTW(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetW(t, i, VW(b, op.vuimm() & 3)); VDONE; }
    static void VSPLTISB(P&, C& c, Op op) { VARGS; for (int i = 0; i < 16; ++i) t.bytes[i] = static_cast<u8>(op.vsimm()); VDONE; }
    static void VSPLTISH(P&, C& c, Op op) { VARGS; for (int i = 0; i < 8; ++i) VSetH(t, i, static_cast<u16>(op.vsimm())); VDONE; }
    static void VSPLTISW(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetW(t, i, static_cast<u32>(op.vsimm())); VDONE; }
    // Merges interleave the high (elements 0..n/2-1) or low halves of VRA and VRB.
    static void VMRGHB(P&, C& c, Op op) { VARGS; for (int i = 0; i < 8; ++i) { t.bytes[2 * i] = a.bytes[i]; t.bytes[2 * i + 1] = b.bytes[i]; } VDONE; }
    static void VMRGLB(P&, C& c, Op op) { VARGS; for (int i = 0; i < 8; ++i) { t.bytes[2 * i] = a.bytes[8 + i]; t.bytes[2 * i + 1] = b.bytes[8 + i]; } VDONE; }
    static void VMRGHH(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) { VSetH(t, 2 * i, VH(a, i)); VSetH(t, 2 * i + 1, VH(b, i)); } VDONE; }
    static void VMRGLH(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) { VSetH(t, 2 * i, VH(a, 4 + i)); VSetH(t, 2 * i + 1, VH(b, 4 + i)); } VDONE; }
    static void VMRGHW(P&, C& c, Op op) { VARGS; for (int i = 0; i < 2; ++i) { VSetW(t, 2 * i, VW(a, i)); VSetW(t, 2 * i + 1, VW(b, i)); } VDONE; }
    static void VMRGLW(P&, C& c, Op op) { VARGS; for (int i = 0; i < 2; ++i) { VSetW(t, 2 * i, VW(a, 2 + i)); VSetW(t, 2 * i + 1, VW(b, 2 + i)); } VDONE; }
    // Fixed-point <-> float with a power-of-two scale in the VRA field; saturating (VSCR[SAT]).
    static void VCFSX(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetF(t, i, std::ldexp(static_cast<f32>(static_cast<s32>(VW(b, i))), -static_cast<int>(op.vuimm()))); VDONE; }
    static void VCFUX(P&, C& c, Op op) { VARGS; for (int i = 0; i < 4; ++i) VSetF(t, i, std::ldexp(static_cast<f32>(VW(b, i)), -static_cast<int>(op.vuimm()))); VDONE; }
    static void VCTSXS(P&, C& c, Op op) {
        VARGS;
        for (int i = 0; i < 4; ++i) {
            const f64 v = std::ldexp(static_cast<f64>(VF(b, i)), static_cast<int>(op.vuimm()));
            s32 r;
            if (std::isnan(v)) r = 0;
            else if (v >= 2147483647.0) { r = std::numeric_limits<s32>::max(); c.vscr |= 1; }
            else if (v < -2147483648.0) { r = std::numeric_limits<s32>::min(); c.vscr |= 1; }
            else r = static_cast<s32>(std::trunc(v));
            VSetW(t, i, static_cast<u32>(r));
        }
        VDONE;
    }
    static void VCTUXS(P&, C& c, Op op) {
        VARGS;
        for (int i = 0; i < 4; ++i) {
            const f64 v = std::ldexp(static_cast<f64>(VF(b, i)), static_cast<int>(op.vuimm()));
            u32 r;
            if (std::isnan(v) || v <= 0.0) { r = 0; if (v < 0.0) c.vscr |= 1; }
            else if (v >= 4294967295.0) { r = 0xFFFFFFFFu; c.vscr |= 1; }
            else r = static_cast<u32>(std::trunc(v));
            VSetW(t, i, r);
        }
        VDONE;
    }
    static void MFVSCR(P&, C& c, Op op) { v128 t{}; VSetW(t, 3, c.vscr); c.vr[op.rd()] = t; }
    static void MTVSCR(P&, C& c, Op op) { c.vscr = VW(c.vr[op.rb()], 3); }

    // Compares: all-ones / all-zeros per element; Rc sets CR6 = all-true(8) | all-false(2).
    static void VcmpFinish(C& c, Op op, const v128& t) {
        c.vr[op.rd()] = t;
        if (!op.vrc()) return;
        bool all = true, none = true;
        for (u8 byte : t.bytes) {
            all &= byte == 0xFF;
            none &= byte == 0;
        }
        SetCrField(c, 6, (all ? 8u : 0u) | (none ? 2u : 0u));
    }
#define PPU_VCMP(NAME, COUNT, GET, SET, TYPE, CMP)                                   \
    static void NAME(P&, C& c, Op op) {                                              \
        VARGS;                                                                       \
        for (int i = 0; i < COUNT; ++i) {                                            \
            const TYPE x = static_cast<TYPE>(GET(a, i)), y = static_cast<TYPE>(GET(b, i)); \
            SET(t, i, (x CMP y) ? ~0u : 0u);                                         \
        }                                                                            \
        VcmpFinish(c, op, t);                                                        \
    }
    static u8 VB(const v128& v, int i) noexcept { return v.bytes[i]; }
    static void VSetB(v128& v, int i, u32 x) noexcept { v.bytes[i] = static_cast<u8>(x); }
    static void VSetH32(v128& v, int i, u32 x) noexcept { VSetH(v, i, static_cast<u16>(x)); }
    PPU_VCMP(VCMPEQUB, 16, VB, VSetB, u8, ==)
    PPU_VCMP(VCMPEQUH, 8, VH, VSetH32, u16, ==)
    PPU_VCMP(VCMPEQUW, 4, VW, VSetW, u32, ==)
    PPU_VCMP(VCMPGTUB, 16, VB, VSetB, u8, >)
    PPU_VCMP(VCMPGTUH, 8, VH, VSetH32, u16, >)
    PPU_VCMP(VCMPGTUW, 4, VW, VSetW, u32, >)
    PPU_VCMP(VCMPGTSB, 16, VB, VSetB, s8, >)
    PPU_VCMP(VCMPGTSH, 8, VH, VSetH32, s16, >)
    PPU_VCMP(VCMPGTSW, 4, VW, VSetW, s32, >)
    PPU_VCMP(VCMPEQFP, 4, VF, VSetW, f32, ==)
    PPU_VCMP(VCMPGEFP, 4, VF, VSetW, f32, >=)
    PPU_VCMP(VCMPGTFP, 4, VF, VSetW, f32, >)
#undef PPU_VCMP
#undef VARGS
#undef VDONE
};

// =======================================================================================
// PpuInterpreter
// =======================================================================================

const std::array<PpuHandler, kPpuInstrCount>& PpuHandlerTable() noexcept {
    // Generated from the same list as the decoder's enum: a missing handler is a compile
    // error, never a silent hole.
    static constexpr std::array<PpuHandler, kPpuInstrCount> kTable = {
#define PPU_HANDLER(id, name) &Ops::id,
        PPU_INSTRUCTION_LIST(PPU_HANDLER)
#undef PPU_HANDLER
    };
    return kTable;
}

PpuInterpreter::PpuInterpreter(vm::GuestMemory& memory, hle::HleRegistry& hle, lv2::SyscallTable& syscalls,
                               PpuCodeCache& code, const std::atomic<bool>* kill)
    : memory_(memory), hle_(hle), syscalls_(syscalls), code_(code), kill_(kill), base_(memory.base()) {}

PpuInterpreter::~PpuInterpreter() = default;

void PpuInterpreter::InvalidateCode(u32 addr, u64 size) noexcept { code_.Invalidate(addr, size); }

StopInfo PpuInterpreter::Execute(PpuThreadContext& ctx, u64 max_instructions) {
    stop_ = StopInfo{};
    stopped_ = false;
    u64 executed = 0;
    while (executed < max_instructions) {
        const u32 pc = ctx.cia;
        current_raw_ = 0;
        if (pc & 3) [[unlikely]] {
            Stop(ctx, StopReason::InstrFault, pc, false, "misaligned PC");
            break;
        }
        if (kill_ && kill_->load(std::memory_order_relaxed)) [[unlikely]] {
            Stop(ctx, StopReason::Terminated);
            break;
        }
        DecodedPage* page = code_.Lookup(pc >> vm::kPageShift);
        if (!page) [[unlikely]] {
            page = code_.Decode(pc >> vm::kPageShift);
            if (!page) {
                Stop(ctx, StopReason::InstrFault, pc, false, "PC in unmapped or non-readable memory");
                break;
            }
        }
        const u32 slot = (pc >> 2) & 1023;
        const Op op{page->raw[slot]};
        current_raw_ = op.raw;
        if (trace_) [[unlikely]]
            std::printf("[ppu] %08x: %08x  %s\n", pc, op.raw, PpuInstrName(page->id[slot]));

        nia_ = pc + 4;
        page->handler[slot](*this, ctx, op);  // `page` may be retired by the handler (SMC); not touched after
        if (stopped_) [[unlikely]]
            break;  // fault/trap/unimplemented: PC stays on the instruction, state not committed

        ctx.cia = nia_;
        history_[history_count_++ & 63] = pc;
        ++executed;
        if (live_ && !(executed & (PpuLiveStatus::kSampleInterval - 1))) [[unlikely]] {
            live_->pc.store(pc, std::memory_order_relaxed);
            live_->instructions.store(total_executed_ + executed, std::memory_order_relaxed);
        }
        if (ctx.exit_requested) [[unlikely]] {
            Stop(ctx, StopReason::Exited);
            break;
        }
    }
    if (!stopped_) Stop(ctx, StopReason::StepLimit);
    stop_.executed = executed;
    total_executed_ += executed;
    if (live_) {
        live_->pc.store(ctx.cia, std::memory_order_relaxed);
        live_->instructions.store(total_executed_, std::memory_order_relaxed);
    }
    return stop_;
}

void PpuInterpreter::Run(PpuThreadContext& ctx) { Execute(ctx, ~u64{0}); }

std::vector<CallRecord> PpuInterpreter::RecentCalls() const {
    std::vector<CallRecord> out;
    const u64 count = std::min<u64>(call_count_, calls_.size());
    for (u64 i = call_count_ - count; i < call_count_; ++i) out.push_back(calls_[i & 15]);
    return out;
}

std::vector<u32> PpuInterpreter::RecentPcs() const {
    std::vector<u32> out;
    const u64 count = std::min<u64>(history_count_, history_.size());
    for (u64 i = history_count_ - count; i < history_count_; ++i) out.push_back(history_[i & 63]);
    return out;
}

}  // namespace cell::ppu
