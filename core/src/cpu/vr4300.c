// VR4300 interpreter: integer unit, COP0, exceptions, Count / Compare. The exactness reference (docs/DESIGN.md).
//
// Behaviour not fixed by the MIPS III manual follows the VR4300 as documented by n64-systemtest and ares; each such
// case says so in a comment and is to be confirmed by the hardware-derived test ROMs (docs/TEST_ROMS.md).
#include "vr4300.h"
#include <stdio.h>
#include <string.h>
#include "../sys/sys.h"
#include "../jit/jit.h"

Vr g_cpu;
volatile bool g_cpuStop;
int g_plant;
u64 g_vrRunUntil;  // vr_run stops when cycles reach it (the scheduler and interrupts lower it)
static u64 g_traceLeft, g_traceFrom;
int g_traceExc;  // --trace-exc: print every exception
u32 g_watchPa;   // --watch PA: print CPU stores to that word (debugging)
u32 g_trapPcAbove;  // --trap-pc-above PA: report the first fetch at or above PA, with the jumps before it
static u64 g_pcRing[32];
static u32 g_pcPos;
static u64 g_cop0Latch;  // the last value written to any COP0 register: read back from unimplemented ones (ares)
bool g_vrIrqPending;  // an enabled interrupt is pending: taken before the next instruction

#define RS ((op >> 21) & 31)
#define RT ((op >> 16) & 31)
#define RD ((op >> 11) & 31)
#define SA ((op >> 6) & 31)
#define SIMM ((u64)(s64)(s16)(u16)op)
#define UIMM ((u64)(u16)op)
#define GR(n) (c->gpr[n])

void vr_trace_enable(u64 count, u64 fromInstruction) { g_traceLeft = count; g_traceFrom = fromInstruction; }

// ---- Count / Compare ----------------------------------------------------------------------------------------------
u32 vr_count(const Vr* c) { return (u32)((c->cycles - c->countBase) >> 1); }

static void schedule_compare(Vr* c) {
    u64 cur = (c->cycles - c->countBase) >> 1;  // Count, not wrapped
    u64 next = (cur & ~0xFFFFFFFFull) | (u32)c->cop0[C0_COMPARE];
    if (next <= cur) next += 1ull << 32;
    c->compareAt = c->countBase + (next << 1);
    sched_at(EV_COMPARE, c->compareAt);
}

void sched_compare_event(void);
void sched_compare_event(void) {
    vr_set_ip(&g_cpu, 7, true);
    schedule_compare(&g_cpu);
}

// ---- Interrupts and exceptions ------------------------------------------------------------------------------------
void vr_check_irq(Vr* c) {
    u32 st = (u32)c->cop0[C0_STATUS];
    u32 pend = (u32)c->cop0[C0_CAUSE] & st & 0xFF00;
    g_vrIrqPending = pend && (st & ST_IE) && !(st & (ST_EXL | ST_ERL));
    if (g_vrIrqPending) g_vrRunUntil = c->cycles;  // leave vr_run's inner loop promptly
}

void vr_set_ip(Vr* c, int bit, bool on) {
    u64 m = 1ull << (8 + bit);
    if (on) c->cop0[C0_CAUSE] |= m;
    else c->cop0[C0_CAUSE] &= ~m;
    vr_check_irq(c);
}

static void exception_at(Vr* c, int code, int ce, u32 vecOff) {
    if (UNLIKELY(g_traceExc) || (UNLIKELY(g_traceLeft) && c->cycles >= g_traceFrom))
        fprintf(stderr, "  exception %d at %08x: status %08x cause %08x (instruction %llu)\n", code, (unsigned)c->pc,
                (unsigned)c->cop0[C0_STATUS],
                (unsigned)c->cop0[C0_CAUSE], (unsigned long long)c->cycles);
    u32 st = (unsigned)c->cop0[C0_STATUS];
    u64 cause = c->cop0[C0_CAUSE] & ~(0x7Cull | (3ull << 28) | (1ull << 31));
    cause |= (u64)(code << 2) | ((u64)ce << 28);
    if (!(st & ST_EXL)) {
        // EPC and BD are only written when not already inside an exception (VR4300 manual 6.4).
        if (c->inDelay) {
            c->cop0[C0_EPC] = c->pc - 4;
            cause |= 1ull << 31;
        } else {
            c->cop0[C0_EPC] = c->pc;
        }
    } else {
        vecOff = 0x180;  // nested TLB refills take the general vector
        cause |= c->cop0[C0_CAUSE] & (1ull << 31);
    }
    c->cop0[C0_CAUSE] = cause;
    c->cop0[C0_STATUS] = st | ST_EXL;
    u64 base = (st & ST_BEV) ? 0xFFFFFFFFBFC00200ull : 0xFFFFFFFF80000000ull;
    c->pc = base + vecOff;
    c->npc = c->pc + 4;
    c->excTaken = true;
    c->branchPending = false;
    c->nextInDelay = false;
    vr_check_irq(c);
}

void vr_exception(Vr* c, int code, int ce) { exception_at(c, code, ce, 0x180); }

// TLB refill exceptions use offset 0x000 (32-bit addressing) or 0x080 (64-bit); vr4300_tlb.c calls this.
void vr_exception_vec(Vr* c, int code, u32 vecOff);
void vr_exception_vec(Vr* c, int code, u32 vecOff) { exception_at(c, code, 0, vecOff); }

// Address errors also write Context's BadVPN2 (hardware: PeterLemon/N64 Exceptions/Unaligned shows 0x00400000 after a
// fault at 0x80000001). XContext is not confirmed and is left alone.
void vr_address_error(Vr* c, u64 va, bool write) {
    c->cop0[C0_BADVADDR] = va;
    c->cop0[C0_CONTEXT] = (c->cop0[C0_CONTEXT] & ~0x7FFFF0ull) | (((va >> 13) & 0x7FFFF) << 4);
    vr_exception(c, write ? EXC_ADES : EXC_ADEL, 0);
}
#define address_error vr_address_error

// ---- Memory access ------------------------------------------------------------------------------------------------
static inline bool xlat(Vr* c, u64 va, bool write, u32* pa) {
    // kseg0 / kseg1 directly; everything else through vr_translate (TLB, 64-bit segments, mode checks).
    u32 a = (u32)va;
    if (LIKELY(va == SEXT32(a) && (a >> 30) == 2 && !(c->cop0[C0_STATUS] & 0x18) )) {
        *pa = a & 0x1FFFFFFF;
        return true;
    }
    return vr_translate(c, va, write, pa);
}

static bool ld(Vr* c, u64 va, int size, u64* out) {
    if (UNLIKELY(va & (size - 1))) { address_error(c, va, false); return false; }
    u32 pa;
    if (!xlat(c, va, false, &pa)) return false;
    if (LIKELY(pa < g_mem.rdramSize)) {
        switch (size) {
        case 1: *out = m_r8(g_mem.rdram, pa); break;
        case 2: *out = m_r16(g_mem.rdram, pa); break;
        case 4: *out = m_r32(g_mem.rdram, pa); break;
        default: *out = m_r64(g_mem.rdram, pa); break;
        }
        return true;
    }
    switch (size) {
    case 1: *out = bus_r8(pa); break;
    case 2: *out = bus_r16(pa); break;
    case 4: *out = bus_r32(pa); break;
    default: *out = bus_r64(pa); break;
    }
    return true;
}

static bool st(Vr* c, u64 va, int size, u64 v) {
    if (UNLIKELY(va & (size - 1))) { address_error(c, va, true); return false; }
    u32 pa;
    if (!xlat(c, va, true, &pa)) return false;
    if (UNLIKELY(g_watchPa) && pa <= g_watchPa + 3 && pa + (u32)size > g_watchPa)
        fprintf(stderr, "  store %d bytes 0x%llx to 0x%08x at pc %08x (instruction %llu)\n", size, (unsigned long long)v,
                (unsigned)pa, (unsigned)c->pc, (unsigned long long)c->cycles);
    if (LIKELY(pa < g_mem.rdramSize)) {
        if (UNLIKELY(g_codePage[pa >> 12])) jit_code_written(pa, (u32)size);
        switch (size) {
        case 1: m_w8(g_mem.rdram, pa, (u8)v); break;
        case 2: m_w16(g_mem.rdram, pa, (u16)v); break;
        case 4: m_w32(g_mem.rdram, pa, (u32)v); break;
        default: m_w64(g_mem.rdram, pa, v); break;
        }
        return true;
    }
    switch (size) {
    case 1: bus_w8(pa, (u8)v); break;
    case 2: bus_w16(pa, (u16)v); break;
    case 4: bus_w32(pa, (u32)v, 0xFFFFFFFFu); break;
    default: bus_w64(pa, v); break;
    }
    return true;
}

static bool fetch(Vr* c, u64 pc, u32* op) {
    if (UNLIKELY(pc & 3)) { address_error(c, pc, false); return false; }
    u32 pa;
    if (!xlat(c, pc, false, &pa)) return false;
    if (UNLIKELY(g_trapPcAbove) && pa >= g_trapPcAbove && pa < 0x04000000u) {  // --trap-pc-above: first fetch there
        fprintf(stderr, "  fetch at 0x%08x (instruction %llu); last jumps (from>to):", (unsigned)pa,
                (unsigned long long)c->cycles);
        for (int i = 0; i < 32; i++) {
            u64 j = g_pcRing[(g_pcPos + i) & 31];
            fprintf(stderr, " %08x>%08x", (unsigned)(j >> 32), (unsigned)j);
        }
        fprintf(stderr, "\n");
        g_trapPcAbove = 0;
    }
    if (LIKELY(pa < g_mem.rdramSize)) {
        *op = m_r32(g_mem.rdram, pa);
        return true;
    }
    if (UNLIKELY(g_traceExc) && pa < 0x03F00000u) {  // running off the end of RDRAM: show how it got there
        fprintf(stderr, "  fetch from unmapped 0x%08x; last jumps (from>to):", (unsigned)pa);
        for (int i = 0; i < 32; i++) {
            u64 j = g_pcRing[(g_pcPos + i) & 31];
            fprintf(stderr, " %08x>%08x", (unsigned)(j >> 32), (unsigned)j);
        }
        fprintf(stderr, "\n");
        g_traceExc = 0;
    }
    *op = bus_r32(pa);
    return true;
}

// ---- Helpers ------------------------------------------------------------------------------------------------------
static inline bool kernel_mode(const Vr* c) {
    u32 s = (u32)c->cop0[C0_STATUS];
    return (s & (ST_EXL | ST_ERL)) || ((s >> 3) & 3) == 0;
}

// 64-bit operations: always legal in kernel mode; in user / supervisor mode only with UX / SX (VR4300 manual 3.1).
static inline bool allow64(Vr* c) {
    if (LIKELY(kernel_mode(c))) return true;
    u32 s = (u32)c->cop0[C0_STATUS];
    int ksu = (s >> 3) & 3;
    if ((ksu == 2 && (s & ST_UX)) || (ksu == 1 && (s & ST_SX))) return true;
    vr_exception(c, EXC_RI, 0);
    return false;
}

static inline bool cop_usable(Vr* c, int n) {
    u32 s = (u32)c->cop0[C0_STATUS];
    if (n == 0 && kernel_mode(c)) return true;
    if (s & (ST_CU0 << n)) return true;
    vr_exception(c, EXC_CPU, n);
    return false;
}

static inline void branch(Vr* c, bool taken, u64 target) {
    c->nextInDelay = true;
    if (taken) { c->branchPending = true; c->branchTo = target; }
}

static inline void branch_likely(Vr* c, bool taken, u64 target) {
    if (taken) {
        c->nextInDelay = true;
        c->branchPending = true;
        c->branchTo = target;
    } else {
        if (LIKELY(g_plant != 4)) c->npc += 4;  // the delay slot is skipped (nullified)
    }
}

static void mulu128(u64 a, u64 b, u64* hi, u64* lo) {
    u64 a0 = (u32)a, a1 = a >> 32, b0 = (u32)b, b1 = b >> 32;
    u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    u64 mid = (p00 >> 32) + (u32)p01 + (u32)p10;
    *lo = (mid << 32) | (u32)p00;
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

static void muls128(s64 a, s64 b, u64* hi, u64* lo) {
    mulu128((u64)a, (u64)b, hi, lo);
    if (a < 0) *hi -= (u64)b;
    if (b < 0) *hi -= (u64)a;
}

static u32 cop0_random(const Vr* c) {
    // Random counts down once per instruction from 31 to Wired, then wraps (the exact phase depends on pipeline
    // timing the interpreter does not model).
    u32 wired = (u32)c->cop0[C0_WIRED] & 63;
    if (wired > 31) return 31;
    u32 range = 32 - wired;
    return 31 - (u32)((c->cycles - c->cop0[C0_RANDOM]) % range);
}

static u64 mfc0(Vr* c, int r) {
    switch (r) {
    case C0_RANDOM: return cop0_random(c);
    case C0_COUNT: return vr_count(c);
    case 7: case 21: case 22: case 23: case 24: case 25: case 31: return g_cop0Latch;
    default: return c->cop0[r];
    }
}

static void mtc0(Vr* c, int r, u64 v) {
    g_cop0Latch = v;
    switch (r) {
    case C0_INDEX: c->cop0[r] = (c->cop0[r] & 0x80000000u) | (v & 0x3F); break;
    case C0_RANDOM: break;
    case C0_ENTRYLO0: case C0_ENTRYLO1: c->cop0[r] = v & 0x3FFFFFFF; break;
    case C0_CONTEXT: c->cop0[r] = (v & ~0x7FFFFFull) | (c->cop0[r] & 0x7FFFF0ull); break;
    case C0_PAGEMASK: c->cop0[r] = v & 0x01FFE000; break;
    case C0_WIRED:
        c->cop0[r] = v & 63;
        c->cop0[C0_RANDOM] = c->cycles;  // Random restarts at 31
        break;
    case C0_BADVADDR: break;
    case C0_COUNT:
        c->countBase = c->cycles - ((u64)(u32)v << 1);
        schedule_compare(c);
        break;
    case C0_ENTRYHI: c->cop0[r] = v & 0xC00000FFFFFFE0FFull; break;
    case C0_COMPARE:
        c->cop0[r] = (u32)v;
        vr_set_ip(c, 7, false);
        schedule_compare(c);
        break;
    case C0_STATUS:
        c->cop0[r] = (u32)v & 0xFF77FFFFu;
        vr_check_irq(c);
        break;
    case C0_CAUSE:
        c->cop0[r] = (c->cop0[r] & ~0x300ull) | (v & 0x300);
        vr_check_irq(c);
        break;
    case C0_PRID: break;
    case C0_CONFIG: c->cop0[r] = (c->cop0[r] & ~0x0F00800Full) | (v & 0x0F00800F); break;
    case C0_XCONTEXT: c->cop0[r] = (v & ~0x1FFFFFFFFull) | (c->cop0[r] & 0x1FFFFFFF0ull); break;
    case C0_PARITYERR: c->cop0[r] = v & 0xFF; break;
    case C0_CACHEERR: break;
    case C0_TAGLO: c->cop0[r] = v & 0x0FFFFFC0; break;
    case C0_TAGHI: c->cop0[r] = 0; break;
    default: c->cop0[r] = v; break;
    }
}

// ---- The instruction set ------------------------------------------------------------------------------------------
static void op_special(Vr* c, u32 op) {
    u64 rs = GR(RS), rt = GR(RT);
    switch (op & 63) {
    case 0x00: GR(RD) = SEXT32((u32)rt << SA); break;                     // SLL
    case 0x02: GR(RD) = SEXT32((u32)rt >> SA); break;                     // SRL
    // SRA / SRAV shift the whole 64-bit register, then keep the low word sign-extended (VR4300: n64-systemtest, ares).
    case 0x03:                                                            // SRA
        GR(RD) = UNLIKELY(g_plant == 5) ? (u64)((s64)(s32)rt >> SA) : SEXT32((u32)((s64)rt >> SA));
        break;
    case 0x04: GR(RD) = SEXT32((u32)rt << (rs & 31)); break;              // SLLV
    case 0x06: GR(RD) = SEXT32((u32)rt >> (rs & 31)); break;              // SRLV
    case 0x07: GR(RD) = SEXT32((u32)((s64)rt >> (rs & 31))); break;       // SRAV
    case 0x08: branch(c, true, rs); break;                                // JR
    case 0x09: {                                                          // JALR
        u64 target = rs;
        GR(RD) = c->pc + 8;
        branch(c, true, target);
        break;
    }
    case 0x0C: vr_exception(c, EXC_SYS, 0); break;                        // SYSCALL
    case 0x0D: vr_exception(c, EXC_BP, 0); break;                         // BREAK
    case 0x0F: break;                                                     // SYNC
    case 0x10: GR(RD) = c->hi; break;                                     // MFHI
    case 0x11: c->hi = rs; break;                                         // MTHI
    case 0x12: GR(RD) = c->lo; break;                                     // MFLO
    case 0x13: c->lo = rs; break;                                         // MTLO
    case 0x14: if (allow64(c)) GR(RD) = rt << (rs & 63); break;           // DSLLV
    case 0x16: if (allow64(c)) GR(RD) = rt >> (rs & 63); break;           // DSRLV
    case 0x17: if (allow64(c)) GR(RD) = (u64)((s64)rt >> (rs & 63)); break;  // DSRAV
    case 0x18: {                                                          // MULT
        s64 p = (s64)(s32)rs * (s64)(s32)rt;
        c->lo = SEXT32((u32)p);
        c->hi = SEXT32((u32)((u64)p >> 32));
        break;
    }
    case 0x19: {                                                          // MULTU
        u64 p = (u64)(u32)rs * (u64)(u32)rt;
        c->lo = SEXT32((u32)p);
        c->hi = SEXT32((u32)(p >> 32));
        break;
    }
    // Division by zero and the one overflowing case give the VR4300's results, without an exception (ares).
    case 0x1A: {                                                          // DIV
        s32 a = (s32)rs, b = (s32)rt;
        if (b == 0) { c->lo = a < 0 ? 1 : ~0ull; c->hi = SEXT32((u32)a); }
        else if (a == INT32_MIN && b == -1) { c->lo = SEXT32((u32)a); c->hi = 0; }
        else { c->lo = SEXT32((u32)(a / b)); c->hi = SEXT32((u32)(a % b)); }
        break;
    }
    case 0x1B: {                                                          // DIVU
        u32 a = (u32)rs, b = (u32)rt;
        if (b == 0) { c->lo = ~0ull; c->hi = SEXT32(a); }
        else { c->lo = SEXT32(a / b); c->hi = SEXT32(a % b); }
        break;
    }
    case 0x1C: if (allow64(c)) muls128((s64)rs, (s64)rt, &c->hi, &c->lo); break;  // DMULT
    case 0x1D: if (allow64(c)) mulu128(rs, rt, &c->hi, &c->lo); break;            // DMULTU
    case 0x1E: {                                                                  // DDIV
        if (!allow64(c)) break;
        s64 a = (s64)rs, b = (s64)rt;
        if (b == 0) { c->lo = a < 0 ? 1 : ~0ull; c->hi = (u64)a; }
        else if (a == INT64_MIN && b == -1) { c->lo = (u64)a; c->hi = 0; }
        else { c->lo = (u64)(a / b); c->hi = (u64)(a % b); }
        break;
    }
    case 0x1F:                                                                    // DDIVU
        if (!allow64(c)) break;
        if (rt == 0) { c->lo = ~0ull; c->hi = rs; }
        else { c->lo = rs / rt; c->hi = rs % rt; }
        break;
    case 0x20: {                                                          // ADD
        u32 r = (u32)rs + (u32)rt;
        if ((s32)((~((u32)rs ^ (u32)rt)) & ((u32)rs ^ r)) < 0) { vr_exception(c, EXC_OV, 0); break; }
        GR(RD) = SEXT32(r);
        break;
    }
    case 0x21: GR(RD) = SEXT32((u32)rs + (u32)rt); break;                 // ADDU
    case 0x22: {                                                          // SUB
        u32 r = (u32)rs - (u32)rt;
        if ((s32)(((u32)rs ^ (u32)rt) & ((u32)rs ^ r)) < 0) { vr_exception(c, EXC_OV, 0); break; }
        GR(RD) = SEXT32(r);
        break;
    }
    case 0x23: GR(RD) = SEXT32((u32)rs - (u32)rt); break;                 // SUBU
    case 0x24: GR(RD) = rs & rt; break;                                   // AND
    case 0x25: GR(RD) = rs | rt; break;                                   // OR
    case 0x26: GR(RD) = rs ^ rt; break;                                   // XOR
    case 0x27: GR(RD) = ~(rs | rt); break;                                // NOR
    case 0x2A: GR(RD) = (s64)rs < (s64)rt; break;                         // SLT
    case 0x2B: GR(RD) = rs < rt; break;                                   // SLTU
    case 0x2C: {                                                          // DADD
        if (!allow64(c)) break;
        u64 r = rs + rt;
        if ((s64)(~(rs ^ rt) & (rs ^ r)) < 0) { vr_exception(c, EXC_OV, 0); break; }
        GR(RD) = r;
        break;
    }
    case 0x2D: if (allow64(c)) GR(RD) = rs + rt; break;                   // DADDU
    case 0x2E: {                                                          // DSUB
        if (!allow64(c)) break;
        u64 r = rs - rt;
        if ((s64)((rs ^ rt) & (rs ^ r)) < 0) { vr_exception(c, EXC_OV, 0); break; }
        GR(RD) = r;
        break;
    }
    case 0x2F:                                                            // DSUBU
        if (!allow64(c)) break;
        GR(RD) = rs - rt;
        if (UNLIKELY(g_plant == 1) && (rs & 0xFF) == 0x55) GR(RD) ^= 1;
        break;
    case 0x30: if ((s64)rs >= (s64)rt) vr_exception(c, EXC_TR, 0); break;  // TGE
    case 0x31: if (rs >= rt) vr_exception(c, EXC_TR, 0); break;            // TGEU
    case 0x32: if ((s64)rs < (s64)rt) vr_exception(c, EXC_TR, 0); break;   // TLT
    case 0x33: if (rs < rt) vr_exception(c, EXC_TR, 0); break;             // TLTU
    case 0x34: if (rs == rt) vr_exception(c, EXC_TR, 0); break;            // TEQ
    case 0x36: if (rs != rt) vr_exception(c, EXC_TR, 0); break;            // TNE
    case 0x38: if (allow64(c)) GR(RD) = rt << SA; break;                   // DSLL
    case 0x3A: if (allow64(c)) GR(RD) = rt >> SA; break;                   // DSRL
    case 0x3B: if (allow64(c)) GR(RD) = (u64)((s64)rt >> SA); break;       // DSRA
    case 0x3C: if (allow64(c)) GR(RD) = rt << (SA + 32); break;            // DSLL32
    case 0x3E: if (allow64(c)) GR(RD) = rt >> (SA + 32); break;            // DSRL32
    case 0x3F: if (allow64(c)) GR(RD) = (u64)((s64)rt >> (SA + 32)); break;  // DSRA32
    default: vr_exception(c, EXC_RI, 0); break;
    }
}

static void op_regimm(Vr* c, u32 op) {
    s64 rs = (s64)GR(RS);
    u64 target = c->pc + 4 + (SIMM << 2);
    u64 imm = SIMM;
    switch (RT) {
    case 0x00: branch(c, rs < 0, target); break;                          // BLTZ
    case 0x01: branch(c, rs >= 0, target); break;                         // BGEZ
    case 0x02: branch_likely(c, rs < 0, target); break;                   // BLTZL
    case 0x03: branch_likely(c, rs >= 0, target); break;                  // BGEZL
    case 0x08: if (rs >= (s64)imm) vr_exception(c, EXC_TR, 0); break;     // TGEI
    case 0x09: if ((u64)rs >= imm) vr_exception(c, EXC_TR, 0); break;     // TGEIU
    case 0x0A: if (rs < (s64)imm) vr_exception(c, EXC_TR, 0); break;      // TLTI
    case 0x0B: if ((u64)rs < imm) vr_exception(c, EXC_TR, 0); break;      // TLTIU
    case 0x0C: if ((u64)rs == imm) vr_exception(c, EXC_TR, 0); break;     // TEQI
    case 0x0E: if ((u64)rs != imm) vr_exception(c, EXC_TR, 0); break;     // TNEI
    case 0x10: GR(31) = c->pc + 8; branch(c, rs < 0, target); break;      // BLTZAL
    case 0x11: GR(31) = c->pc + 8; branch(c, rs >= 0, target); break;     // BGEZAL
    case 0x12: GR(31) = c->pc + 8; branch_likely(c, rs < 0, target); break;   // BLTZALL
    case 0x13: GR(31) = c->pc + 8; branch_likely(c, rs >= 0, target); break;  // BGEZALL
    default: vr_exception(c, EXC_RI, 0); break;
    }
}

static void op_cop0(Vr* c, u32 op) {
    if (!cop_usable(c, 0)) return;
    switch (RS) {
    case 0x00: GR(RT) = SEXT32((u32)mfc0(c, RD)); break;                  // MFC0
    case 0x01: if (allow64(c)) GR(RT) = mfc0(c, RD); break;               // DMFC0
    case 0x04: mtc0(c, RD, SEXT32((u32)GR(RT))); break;                   // MTC0
    case 0x05: if (allow64(c)) mtc0(c, RD, GR(RT)); break;                // DMTC0
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        switch (op & 63) {
        case 0x01: vr_tlbr(c); break;
        case 0x02: vr_tlbwi(c); break;
        case 0x06: vr_tlbwr(c); break;
        case 0x08: vr_tlbp(c); break;
        case 0x18: {                                                      // ERET (no delay slot)
            u32 s = (u32)c->cop0[C0_STATUS];
            if (s & ST_ERL) {
                c->npc = c->cop0[C0_ERROREPC];
                c->cop0[C0_STATUS] = s & ~ST_ERL;
            } else {
                c->npc = c->cop0[C0_EPC];
                c->cop0[C0_STATUS] = s & ~ST_EXL;
            }
            c->llbit = false;
            if (UNLIKELY(g_traceExc)) {
                g_pcRing[g_pcPos++ & 31] = (c->pc << 32) | (u32)c->npc;
                if ((u32)c->npc >= 0x80400000u && (u32)c->npc < 0x80800000u)
                    fprintf(stderr, "  eret to %08x (instruction %llu)\n", (unsigned)c->npc,
                            (unsigned long long)c->cycles);
            }
            vr_check_irq(c);
            break;
        }
        default: vr_exception(c, EXC_RI, 0); break;
        }
        break;
    default: vr_exception(c, EXC_RI, 0); break;
    }
}

// The VR4300 has no coprocessor 2; with CU2 set, its moves read and write a latch (n64-systemtest, ares).
static u64 g_cop2Latch;
static void op_cop2(Vr* c, u32 op) {
    if (!cop_usable(c, 2)) return;
    switch (RS) {
    case 0x00: GR(RT) = SEXT32((u32)g_cop2Latch); break;
    case 0x01: GR(RT) = g_cop2Latch; break;
    case 0x02: GR(RT) = SEXT32((u32)g_cop2Latch); break;
    case 0x04: case 0x05: case 0x06: g_cop2Latch = GR(RT); break;
    default: vr_exception(c, EXC_RI, 0); break;
    }
}

static void op_load_lr(Vr* c, u32 op) {
    u64 va = GR(RS) + SIMM;
    u64 rt = GR(RT);
    u64 w;
    switch (op >> 26) {
    case 0x22: {  // LWL
        if (!ld(c, va & ~3ull, 4, &w)) return;
        int sh = (int)(va & 3) * 8;
        u32 keep = sh ? ((1u << sh) - 1) : 0;
        GR(RT) = SEXT32(((u32)w << sh) | ((u32)rt & keep));
        break;
    }
    case 0x26: {  // LWR: the register's upper half is kept unless the whole word is loaded (ares)
        if (!ld(c, va & ~3ull, 4, &w)) return;
        int sh = (int)(3 - (va & 3)) * 8;
        if (sh == 0) { GR(RT) = SEXT32((u32)w); break; }
        u64 keep = ~0ull << (32 - sh);
        GR(RT) = (rt & keep) | ((u32)w >> sh);
        break;
    }
    case 0x1A: {  // LDL
        if (!allow64(c) || !ld(c, va & ~7ull, 8, &w)) return;
        int sh = (int)(va & 7) * 8;
        u64 keep = sh ? ((1ull << sh) - 1) : 0;
        GR(RT) = (w << sh) | (rt & keep);
        break;
    }
    case 0x1B: {  // LDR
        if (!allow64(c) || !ld(c, va & ~7ull, 8, &w)) return;
        int sh = (int)(7 - (va & 7)) * 8;
        u64 keep = sh ? ~(~0ull >> sh) : 0;
        if (UNLIKELY(g_plant == 2)) keep <<= 8;
        GR(RT) = (w >> sh) | (rt & keep);
        break;
    }
    }
}

static void op_store_lr(Vr* c, u32 op) {
    u64 va = GR(RS) + SIMM;
    u64 rt = GR(RT);
    u64 w;
    switch (op >> 26) {
    case 0x2A: {  // SWL
        if (!ld(c, va & ~3ull, 4, &w)) return;
        int sh = (int)(va & 3) * 8;
        u32 m = 0xFFFFFFFFu >> sh;
        st(c, va & ~3ull, 4, ((u32)w & ~m) | ((u32)rt >> sh));
        break;
    }
    case 0x2E: {  // SWR
        if (!ld(c, va & ~3ull, 4, &w)) return;
        int sh = (int)(3 - (va & 3)) * 8;
        u32 m = 0xFFFFFFFFu << sh;
        st(c, va & ~3ull, 4, ((u32)w & ~m) | ((u32)rt << sh));
        break;
    }
    case 0x2C: {  // SDL
        if (!allow64(c) || !ld(c, va & ~7ull, 8, &w)) return;
        int sh = (int)(va & 7) * 8;
        u64 m = ~0ull >> sh;
        st(c, va & ~7ull, 8, (w & ~m) | (rt >> sh));
        break;
    }
    case 0x2D: {  // SDR
        if (!allow64(c) || !ld(c, va & ~7ull, 8, &w)) return;
        int sh = (int)(7 - (va & 7)) * 8;
        u64 m = ~0ull << sh;
        st(c, va & ~7ull, 8, (w & ~m) | (rt << sh));
        break;
    }
    }
}

static void exec(Vr* c, u32 op) {
    u64 rs = GR(RS);
    u64 v;
    switch (op >> 26) {
    case 0x00: op_special(c, op); break;
    case 0x01: op_regimm(c, op); break;
    case 0x02: branch(c, true, ((c->pc + 4) & ~0x0FFFFFFFull) | ((op & 0x03FFFFFF) << 2)); break;  // J
    case 0x03:                                                                                     // JAL
        GR(31) = c->pc + 8;
        branch(c, true, ((c->pc + 4) & ~0x0FFFFFFFull) | ((op & 0x03FFFFFF) << 2));
        break;
    case 0x04: branch(c, rs == GR(RT), c->pc + 4 + (SIMM << 2)); break;               // BEQ
    case 0x05: branch(c, rs != GR(RT), c->pc + 4 + (SIMM << 2)); break;               // BNE
    case 0x06: branch(c, (s64)rs <= 0, c->pc + 4 + (SIMM << 2)); break;               // BLEZ
    case 0x07: branch(c, (s64)rs > 0, c->pc + 4 + (SIMM << 2)); break;                // BGTZ
    case 0x08: {                                                                      // ADDI
        u32 a = (u32)rs, b = (u32)SIMM, r = a + b;
        if ((s32)(~(a ^ b) & (a ^ r)) < 0) { vr_exception(c, EXC_OV, 0); break; }
        GR(RT) = SEXT32(r);
        break;
    }
    case 0x09: GR(RT) = SEXT32((u32)rs + (u32)SIMM); break;                           // ADDIU
    case 0x0A: GR(RT) = (s64)rs < (s64)SIMM; break;                                   // SLTI
    case 0x0B: GR(RT) = rs < SIMM; break;                                             // SLTIU
    case 0x0C: GR(RT) = rs & UIMM; break;                                             // ANDI
    case 0x0D: GR(RT) = rs | UIMM; break;                                             // ORI
    case 0x0E: GR(RT) = rs ^ UIMM; break;                                             // XORI
    case 0x0F: GR(RT) = SEXT32((u32)UIMM << 16); break;                               // LUI
    case 0x10: op_cop0(c, op); break;
    case 0x11: if (cop_usable(c, 1)) vr_cop1(c, op); break;
    case 0x12: op_cop2(c, op); break;
    case 0x14: branch_likely(c, rs == GR(RT), c->pc + 4 + (SIMM << 2)); break;        // BEQL
    case 0x15: branch_likely(c, rs != GR(RT), c->pc + 4 + (SIMM << 2)); break;        // BNEL
    case 0x16: branch_likely(c, (s64)rs <= 0, c->pc + 4 + (SIMM << 2)); break;        // BLEZL
    case 0x17: branch_likely(c, (s64)rs > 0, c->pc + 4 + (SIMM << 2)); break;         // BGTZL
    case 0x18: {                                                                      // DADDI
        if (!allow64(c)) break;
        u64 b = SIMM, r = rs + b;
        if ((s64)(~(rs ^ b) & (rs ^ r)) < 0) { vr_exception(c, EXC_OV, 0); break; }
        GR(RT) = r;
        break;
    }
    case 0x19: if (allow64(c)) GR(RT) = rs + SIMM; break;                             // DADDIU
    case 0x1A: case 0x1B: case 0x22: case 0x26: op_load_lr(c, op); break;             // LDL LDR LWL LWR
    case 0x20: if (ld(c, rs + SIMM, 1, &v)) GR(RT) = SEXT8(v); break;                 // LB
    case 0x21: if (ld(c, rs + SIMM, 2, &v)) GR(RT) = SEXT16(v); break;                // LH
    case 0x23: if (ld(c, rs + SIMM, 4, &v)) GR(RT) = SEXT32(v); break;                // LW
    case 0x24: if (ld(c, rs + SIMM, 1, &v)) GR(RT) = v; break;                        // LBU
    case 0x25: if (ld(c, rs + SIMM, 2, &v)) GR(RT) = v; break;                        // LHU
    case 0x27: if (allow64(c) && ld(c, rs + SIMM, 4, &v)) GR(RT) = v; break;          // LWU
    case 0x28: st(c, rs + SIMM, 1, GR(RT)); break;                                    // SB
    case 0x29: st(c, rs + SIMM, 2, GR(RT)); break;                                    // SH
    case 0x2A: case 0x2E: case 0x2C: case 0x2D: op_store_lr(c, op); break;            // SWL SWR SDL SDR
    case 0x2B: st(c, rs + SIMM, 4, GR(RT)); break;                                    // SW
    case 0x2F: if (!kernel_mode(c)) cop_usable(c, 0); break;                          // CACHE (no cache model)
    case 0x30: {                                                                      // LL
        u32 pa;
        if (!ld(c, rs + SIMM, 4, &v)) break;
        if (xlat(c, rs + SIMM, false, &pa)) c->cop0[C0_LLADDR] = pa >> 4;
        GR(RT) = SEXT32(v);
        c->llbit = true;
        break;
    }
    case 0x31:                                                                        // LWC1
        if (cop_usable(c, 1) && ld(c, rs + SIMM, 4, &v)) vr_fpr_write_w(c, RT, (u32)v);
        break;
    case 0x32: case 0x3A: if (cop_usable(c, 2)) vr_exception(c, EXC_RI, 0); break;    // LWC2 SWC2
    case 0x34: {                                                                      // LLD
        u32 pa;
        if (!allow64(c) || !ld(c, rs + SIMM, 8, &v)) break;
        if (xlat(c, rs + SIMM, false, &pa)) c->cop0[C0_LLADDR] = pa >> 4;
        GR(RT) = v;
        c->llbit = true;
        break;
    }
    case 0x35: if (cop_usable(c, 1) && ld(c, rs + SIMM, 8, &v)) vr_fpr_write_d(c, RT, v); break;  // LDC1
    case 0x36: case 0x3E: if (cop_usable(c, 2)) vr_exception(c, EXC_RI, 0); break;    // LDC2 SDC2
    case 0x37: if (allow64(c) && ld(c, rs + SIMM, 8, &v)) GR(RT) = v; break;          // LD
    case 0x38: {                                                                      // SC
        u64 va = rs + SIMM;
        if (c->llbit) {
            if (st(c, va, 4, GR(RT))) GR(RT) = 1;
        } else {
            if (va & 3) { address_error(c, va, true); break; }
            GR(RT) = 0;
        }
        break;
    }
    case 0x39: if (cop_usable(c, 1)) st(c, rs + SIMM, 4, vr_fpr_read_w(c, RT)); break;  // SWC1
    case 0x3C: {                                                                      // SCD
        if (!allow64(c)) break;
        u64 va = rs + SIMM;
        if (c->llbit) {
            if (st(c, va, 8, GR(RT))) GR(RT) = 1;
        } else {
            if (va & 7) { address_error(c, va, true); break; }
            GR(RT) = 0;
        }
        break;
    }
    case 0x3D: if (cop_usable(c, 1)) st(c, rs + SIMM, 8, vr_fpr_read_d(c, RT)); break;  // SDC1
    case 0x3F: if (allow64(c)) st(c, rs + SIMM, 8, GR(RT)); break;                    // SD
    default: vr_exception(c, EXC_RI, 0); break;
    }
}

// ---- Stepping -----------------------------------------------------------------------------------------------------
// The JIT's way to run one instruction it does not translate (jit.c): exactly what vr_step does after fetching, with
// the cycle count already advanced by the JIT and c->pc at the instruction. Returns nonzero when the compiled block
// must end after this instruction, with pc / npc set for the dispatcher: an exception, a COP0 instruction (it may
// change the mode or enable an interrupt), compiled code overwritten, or the scheduler's limit now inside the rest of
// the block (`remaining` instructions). In a delay slot only an exception ends it: the block ends after the slot.
static int jit_fallback(Vr* c, u32 op, u32 remaining, u32 inDelay);
int vr_jit_fallback(Vr* c, u32 op, u32 remaining, u32 inDelay) {  // (the profiler's phase around it)
    g_phase = PH_INTERP;
    int r = jit_fallback(c, op, remaining, inDelay);
    g_phase = PH_BLOCK;
    return r;
}
static int jit_fallback(Vr* c, u32 op, u32 remaining, u32 inDelay) {
    c->inDelay = inDelay != 0;
    c->nextInDelay = false;
    c->excTaken = false;
    c->branchPending = false;
    u64 after = c->pc + 4;
    c->npc = after;
    g_jitCodeHit = false;
    {
        u32 p = op >> 26;
        u32 key = p == 0 ? 64 + (op & 63) : p == 1 ? 128 + ((op >> 16) & 31) : p == 0x11 ? 160 + ((op >> 21) & 31) : p;
        g_jitStats.fallbackOps[key]++;  // what the JIT should translate next (n64emu --stats)
    }
    exec(c, op);
    c->gpr[0] = 0;
    if (c->excTaken) return 1;
    if (inDelay) return 0;
    if ((op >> 26) == 0x10 || g_jitCodeHit || c->cycles + remaining > g_vrRunUntil || g_vrIrqPending) {
        c->pc = c->npc;  // ERET set npc to its target; everything else continues at pc + 4
        c->npc = c->pc + 4;
        return 1;
    }
    return 0;
}

void vr_step(Vr* c) {
    c->inDelay = c->nextInDelay;
    c->nextInDelay = false;
    c->excTaken = false;
    c->cycles += 1;
    if (UNLIKELY(g_vrIrqPending)) {
        exception_at(c, EXC_INT, 0, 0x180);
        return;
    }
    u32 op;
    if (fetch(c, c->pc, &op)) {
        if (UNLIKELY(g_traceLeft) && c->cycles >= g_traceFrom) {
            char buf[96];
            u64 before[32];
            g_traceLeft--;
            fprintf(stderr, "%08x: %08x  %-32s", (unsigned)c->pc, (unsigned)op, vr_disasm(op, c->pc, buf, sizeof buf));
            for (int i = 0; i < 32; i++) before[i] = c->gpr[i];
            exec(c, op);
            for (int i = 1; i < 32; i++)
                if (c->gpr[i] != before[i]) fprintf(stderr, " r%d=%016llx", i, (unsigned long long)c->gpr[i]);
            fprintf(stderr, "\n");
        } else {
            exec(c, op);
        }
    }
    c->gpr[0] = 0;
    if (c->excTaken) return;
    c->pc = c->npc;
    if (c->branchPending) {
        if (UNLIKELY(g_traceExc)) g_pcRing[g_pcPos++ & 31] = ((c->pc - 4) << 32) | (u32)c->branchTo;
        c->npc = c->branchTo;
        c->branchPending = false;
    } else {
        c->npc = c->pc + 4;
    }
}

void vr_run(Vr* c, u64 untilCycles) {
    if (g_jitEnabled) { jit_run(c, untilCycles); return; }
    g_vrRunUntil = untilCycles;
    while (c->cycles < g_vrRunUntil && !g_cpuStop) vr_step(c);
}

// The scheduler lowers the run limit when an event lands before it (a DMA started by this instruction, say).
void vr_run_limit(u64 when);
void vr_run_limit(u64 when) {
    if (when < g_vrRunUntil) g_vrRunUntil = when;
}

void vr_reset(Vr* c) {
    memset(c, 0, sizeof *c);
    c->cop0[C0_STATUS] = 0x34000000;   // CU0, CU1, FR (the state IPL3 leaves; rom.c sets it for the boot path)
    c->cop0[C0_CONFIG] = 0x7006E463;
    c->cop0[C0_PRID] = 0x00000B22;
    c->cop0[C0_COMPARE] = 0;  // as after IPL3 on hardware (PeterLemon/N64 CP0/COP0Register)
    c->cop0[C0_RANDOM] = 0;
    c->fcr31 = 0;
    // Power-on TLB contents are random, so nothing matches; all-zero entries would match low addresses as "invalid"
    // (a refill on hardware: PeterLemon/N64 Exceptions/TLB). Park them in kseg0, which is never translated, as
    // libultra's osUnmapTLBAll does.
    for (int i = 0; i < 32; i++) c->tlb[i].entryHi = 0xFFFFFFFF80000000ull + (u64)i * 0x2000;
    c->pc = 0xFFFFFFFFBFC00000ull;
    c->npc = c->pc + 4;
    g_cop0Latch = 0;
    g_cop2Latch = 0;
    g_vrIrqPending = false;
    schedule_compare(c);
}
