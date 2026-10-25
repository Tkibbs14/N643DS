// VR4300 -> ARMv6K JIT, first version (jit.h). Blocks are straight-line guest code ending after a branch and its
// delay slot (or after a COP0 instruction, or at MAX_INSNS). Translated natively: the integer ALU (32- and 64-bit,
// shifts by constants), multiplies, HI / LO moves, loads and stores (inline RDRAM fast path), branches and jumps.
// Everything else runs through vr_jit_fallback, the interpreter's own code.
//
// Host registers in a block: r11 = &g_cpu, r10 = RDRAM, r9 = g_codePage, r6:r7 = the cycle count at block entry,
// r4 = branch taken, r5:r8 = a register jump's target; r0-r3, r12 scratch. Guest registers live in g_cpu (no register
// cache yet). Cycles are written back only before calls and at exits: entry + index + 1 before instruction `index`
// calls out (the interpreter counts an instruction before running it), entry + count at an exit.
#include "jit.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../cpu/vr4300.h"
#include "../sys/sys.h"

bool g_jitEnabled;
bool g_jitCodeHit;
JitStats g_jitStats;

#if !defined(__arm__)

bool jit_available(void) { return false; }
bool jit_init(void) { return false; }
void jit_run(struct Vr* c, u64 untilCycles) { (void)c; (void)untilCycles; }
void jit_flush(void) {}
void jit_code_written(u32 pa, u32 len) {
    (void)len;
    g_codePage[pa >> 12] = 0;
    g_jitCodeHit = true;
}

#else

#include "arm_emit.h"

#define CODE_BYTES (8u << 20)
#define MAX_BLOCKS 65536
#define MAX_INSNS 128
#define NPAGES (RDRAM_MAX >> 12)

typedef struct {
    void (*code)(Vr*);
    u32 n;      // guest instructions, for the budget check
    u32 va;     // the guest address it was compiled for (TLB-mapped code: a physical page may appear at several)
    bool idle;  // a branch to itself with a NOP in its delay slot: fast-forwarded to the next event
} Block;

static u32* g_code;
static u32 g_codeUsed, g_codeCap;  // in words
static Block g_blocks[MAX_BLOCKS];
static u32 g_nblocks;
static Block** g_map[NPAGES];      // per RDRAM page: one entry per word (the block starting there)
static Block g_interp;             // code NULL: this address runs in the interpreter
static u32 g_offMask;              // N64_JIT_OFF (op_class)

#define OFF_GPR(r, half) ((u32)(offsetof(Vr, gpr) + 8u * (r) + 4u * (half)))
#define OFF_HI(half) ((u32)(offsetof(Vr, hi) + 4u * (half)))
#define OFF_LO(half) ((u32)(offsetof(Vr, lo) + 4u * (half)))
#define OFF_PC(half) ((u32)(offsetof(Vr, pc) + 4u * (half)))
#define OFF_NPC(half) ((u32)(offsetof(Vr, npc) + 4u * (half)))
#define OFF_CYC(half) ((u32)(offsetof(Vr, cycles) + 4u * (half)))

bool jit_available(void) { return true; }

bool jit_init(void) {
    if (g_code) return true;
    if (getenv("N64_JIT_OFF")) g_offMask = (u32)strtoul(getenv("N64_JIT_OFF"), NULL, 0);
    g_code = jit_host_alloc_exec(CODE_BYTES);
    if (!g_code) return false;
    g_codeCap = CODE_BYTES / 4;
    g_codeUsed = 0;
    return true;
}

void jit_flush(void) {
    for (u32 p = 0; p < NPAGES; p++)
        if (g_map[p]) memset(g_map[p], 0, 1024 * sizeof(Block*));
    memset(g_codePage, 0, sizeof g_codePage);
    g_codeUsed = 0;
    g_nblocks = 0;
    g_jitStats.flushes++;
}

static void invalidate_page(u32 p) {
    if (g_map[p]) memset(g_map[p], 0, 1024 * sizeof(Block*));
    g_codePage[p] = 0;
    g_jitStats.invalidations++;
}

// RDRAM written where code was compiled: drop the page's blocks, and the previous page's, whose blocks may run into
// this one (a block is at most MAX_INSNS words, so it spans two pages at most).
void jit_code_written(u32 pa, u32 len) {
    u32 p0 = pa >> 12, p1 = (pa + (len ? len : 1) - 1) >> 12;
    for (u32 p = p0; p <= p1 && p < NPAGES; p++) {
        invalidate_page(p);
        if (p) invalidate_page(p - 1);
    }
    g_jitCodeHit = true;
}

// ---- Emission helpers ----------------------------------------------------------------------------------------------
typedef struct {
    Emit e;
    u32 va0, n;        // the block's first guest address and length
    u32 exits[512];    // branches to the shared return (helper said: leave)
    u32 nexits;
} Ctx;

static void ldrsb_reg(Emit* e, int rt, int rn, int rm) { E(e, cc(C_AL) | 0x019000D0u | ((u32)rn << 16) | ((u32)rt << 12) | (u32)rm); }
static void ldrb_lsr12(Emit* e, int rt, int rn, int rm) {  // ldrb rt, [rn, rm, lsr #12]
    E(e, cc(C_AL) | 0x07D00000u | ((u32)rn << 16) | ((u32)rt << 12) | (12u << 7) | ((u32)LSR << 5) | (u32)rm);
}
static void imm_op(Emit* e, int opc, int s, int rd, int rn, u32 v, int tmp) {  // rd = rn OP v
    u32 enc;
    if (arm_imm(v, &enc)) { dp_imm(e, opc, s, rd, rn, enc); return; }
    load_const(e, tmp, v);
    dp_reg(e, opc, s, rd, rn, tmp);
}
static void cmp_imm(Emit* e, int rn, u32 v, int tmp) { imm_op(e, CMP, 1, 0, rn, v, tmp); }

static void ldg(Emit* e, int host, int g, int half) {
    if (g == 0) load_const(e, host, 0);
    else ldr_imm(e, host, R11, OFF_GPR(g, half));
}
static void stg(Emit* e, int g, int half, int host) {
    if (g) str_imm(e, host, R11, OFF_GPR(g, half));
}
// A 32-bit result in `lo`, stored sign-extended (uses `tmp`).
static void st32(Emit* e, int g, int lo, int tmp) {
    if (!g) return;
    str_imm(e, lo, R11, OFF_GPR(g, 0));
    mov_shift(e, 0, tmp, lo, ASR, 31);
    str_imm(e, tmp, R11, OFF_GPR(g, 1));
}
static void st64(Emit* e, int g, int lo, int hi) {
    if (!g) return;
    str_imm(e, lo, R11, OFF_GPR(g, 0));
    str_imm(e, hi, R11, OFF_GPR(g, 1));
}
static u32 sx_hi(u32 v) { return (v & 0x80000000u) ? 0xFFFFFFFFu : 0; }

static void epilogue(Emit* e) {
    dp_imm(e, ADD, 0, SP, SP, 4);
    pop_regs(e, (1u << R4) | (1u << R5) | (1u << R6) | (1u << R7) | (1u << R8) | (1u << R9) | (1u << R10) |
                    (1u << R11) | (1u << PC));
}

// c->cycles = entry + count
static void emit_cycles(Emit* e, u32 count) {
    u32 enc;
    arm_imm(count, &enc);  // count <= MAX_INSNS + 1 always encodes
    dp_imm(e, ADD, 1, R0, R6, enc);
    dp_imm(e, ADC, 0, R1, R7, 0);
    str_imm(e, R0, R11, OFF_CYC(0));
    str_imm(e, R1, R11, OFF_CYC(1));
}

// Leaves the block for a constant guest address after `count` instructions.
static void emit_exit_const(Ctx* x, u32 target, u32 count) {
    Emit* e = &x->e;
    load_const(e, R0, target);
    load_const(e, R1, sx_hi(target));
    str_imm(e, R0, R11, OFF_PC(0));
    str_imm(e, R1, R11, OFF_PC(1));
    load_const(e, R0, target + 4);
    load_const(e, R1, sx_hi(target + 4));
    str_imm(e, R0, R11, OFF_NPC(0));
    str_imm(e, R1, R11, OFF_NPC(1));
    emit_cycles(e, count);
    epilogue(e);
}

// Leaves the block for the address in r5 (low) : r8 (high).
static void emit_exit_reg(Ctx* x, u32 count) {
    Emit* e = &x->e;
    str_imm(e, R5, R11, OFF_PC(0));
    str_imm(e, R8, R11, OFF_PC(1));
    dp_imm(e, ADD, 1, R0, R5, 4);
    dp_imm(e, ADC, 0, R1, R8, 0);
    str_imm(e, R0, R11, OFF_NPC(0));
    str_imm(e, R1, R11, OFF_NPC(1));
    emit_cycles(e, count);
    epilogue(e);
}

// Runs instruction `i` in the interpreter; leaves the block when it says so.
static void emit_fallback(Ctx* x, u32 op, u32 i, bool inDelay) {
    Emit* e = &x->e;
    u32 va = x->va0 + 4 * i;
    load_const(e, R0, va);
    load_const(e, R1, sx_hi(va));
    str_imm(e, R0, R11, OFF_PC(0));
    str_imm(e, R1, R11, OFF_PC(1));
    emit_cycles(e, i + 1);
    mov_reg(e, R0, R11);
    load_const(e, R1, op);
    load_const(e, R2, x->n - (i + 1));
    load_const(e, R3, inDelay ? 1 : 0);
    load_const(e, R12, (u32)(uintptr_t)vr_jit_fallback);
    blx_reg(e, R12);
    dp_imm(e, CMP, 1, 0, R0, 0);
    if (x->nexits < sizeof x->exits / sizeof x->exits[0]) x->exits[x->nexits++] = branch_c(e, C_NE, 0);
    g_jitStats.fallbacks++;
}

// ---- Loads and stores ----------------------------------------------------------------------------------------------
// Fast path: a sign-extended base, no 32-bit overflow from the offset, kseg0 / kseg1, aligned, inside RDRAM, and for
// stores a page without compiled code. Leaves the physical address in r2; every failure branches to the slow path
// (the interpreter runs the whole instruction).
static u32 emit_address(Ctx* x, u32 op, int size, bool store, u32* slow) {
    Emit* e = &x->e;
    int rs = (op >> 21) & 31;
    s32 imm = (s16)(u16)op;
    u32 ns = 0;
    ldg(e, R0, rs, 0);
    ldg(e, R1, rs, 1);
    dp_reg_c(e, C_AL, CMP, 1, 0, R1, R0, ASR, 31);
    slow[ns++] = branch_c(e, C_NE, 0);
    if (imm) {
        u32 enc;
        if (imm > 0 && arm_imm((u32)imm, &enc)) dp_imm(e, ADD, 1, R0, R0, enc);
        else if (imm < 0 && arm_imm((u32)-imm, &enc)) dp_imm(e, SUB, 1, R0, R0, enc);
        else { load_const(e, R2, (u32)imm); dp_reg(e, ADD, 1, R0, R0, R2); }
        slow[ns++] = branch_c(e, C_VS, 0);
    }
    u32 enc;
    arm_imm(0xC0000000u, &enc);
    dp_imm(e, AND, 0, R2, R0, enc);
    arm_imm(0x80000000u, &enc);
    dp_imm(e, CMP, 1, 0, R2, enc);
    slow[ns++] = branch_c(e, C_NE, 0);
    if (size > 1) {
        arm_imm((u32)size - 1, &enc);
        dp_imm(e, TST, 1, 0, R0, enc);
        slow[ns++] = branch_c(e, C_NE, 0);
    }
    arm_imm(0xE0000000u, &enc);
    dp_imm(e, BIC, 0, R2, R0, enc);
    cmp_imm(e, R2, g_mem.rdramSize, R3);
    slow[ns++] = branch_c(e, C_CS, 0);
    if (store) {
        ldrb_lsr12(e, R3, R9, R2);
        dp_imm(e, CMP, 1, 0, R3, 0);
        slow[ns++] = branch_c(e, C_NE, 0);
    }
    return ns;
}

static void emit_mem(Ctx* x, u32 op, u32 i, bool inDelay) {
    Emit* e = &x->e;
    u32 prim = op >> 26;
    int rt = (op >> 16) & 31;
    static const int sizes[64] = {[0x20] = 1, [0x24] = 1, [0x21] = 2, [0x25] = 2, [0x23] = 4, [0x27] = 4, [0x37] = 8,
                                  [0x28] = 1, [0x29] = 2, [0x2B] = 4, [0x3F] = 8};
    int size = sizes[prim];
    bool store = (prim >= 0x28 && prim <= 0x2B) || prim == 0x3F;  // LD (0x37) is a load
    u32 slow[8];
    u32 ns = emit_address(x, op, size, store, slow);
    if (!store) {
        switch (prim) {
        case 0x20: dp_imm(e, EOR, 0, R2, R2, 3); ldrsb_reg(e, R3, R10, R2); st32(e, rt, R3, R12); break;  // LB
        case 0x24: dp_imm(e, EOR, 0, R2, R2, 3); ldrb_reg(e, R3, R10, R2);                                 // LBU
                   stg(e, rt, 0, R3); load_const(e, R12, 0); stg(e, rt, 1, R12); break;
        case 0x21: dp_imm(e, EOR, 0, R2, R2, 2); ldrsh_reg(e, R3, R10, R2); st32(e, rt, R3, R12); break;  // LH
        case 0x25: dp_imm(e, EOR, 0, R2, R2, 2); ldrh_reg(e, R3, R10, R2);                                 // LHU
                   stg(e, rt, 0, R3); load_const(e, R12, 0); stg(e, rt, 1, R12); break;
        case 0x23: ldr_reg(e, R3, R10, R2); st32(e, rt, R3, R12); break;                                     // LW
        case 0x27: ldr_reg(e, R3, R10, R2); stg(e, rt, 0, R3); load_const(e, R12, 0); stg(e, rt, 1, R12); break;  // LWU
        default:                                                                                             // LD
            dp_reg(e, ADD, 0, R2, R2, R10);
            ldr_imm(e, R3, R2, 0);
            ldr_imm(e, R12, R2, 4);
            st64(e, rt, R12, R3);
            break;
        }
    } else {
        switch (prim) {
        case 0x28: ldg(e, R3, rt, 0); dp_imm(e, EOR, 0, R2, R2, 3); strb_reg(e, R3, R10, R2); break;  // SB
        case 0x29: ldg(e, R3, rt, 0); dp_imm(e, EOR, 0, R2, R2, 2); strh_reg(e, R3, R10, R2); break;  // SH
        case 0x2B: ldg(e, R3, rt, 0); str_reg(e, R3, R10, R2); break;                                 // SW
        default:                                                                                      // SD
            dp_reg(e, ADD, 0, R2, R2, R10);
            ldg(e, R3, rt, 1);
            str_imm(e, R3, R2, 0);
            ldg(e, R3, rt, 0);
            str_imm(e, R3, R2, 4);
            break;
        }
    }
    u32 done = branch_c(e, C_AL, 0);
    for (u32 k = 0; k < ns; k++) patch_branch(e, slow[k], e->n);
    emit_fallback(x, op, i, inDelay);
    patch_branch(e, done, e->n);
}

// ---- Instructions --------------------------------------------------------------------------------------------------
static bool is_branch(u32 op) {
    u32 p = op >> 26;
    if (p == 0) return (op & 63) == 8 || (op & 63) == 9;
    if (p == 1) { u32 rt = (op >> 16) & 31; return rt <= 3 || (rt >= 0x10 && rt <= 0x13); }
    if (p == 0x11) return ((op >> 21) & 31) == 8;  // BC1
    return (p >= 2 && p <= 7) || (p >= 0x14 && p <= 0x17);
}

// N64_JIT_OFF=mask (debugging): instruction classes sent to the interpreter instead of translated, to find which
// translation differs. 1 SLL/SRL/SRA, 2 HI/LO moves, 4 MULT(U), 8 ADDU/SUBU, 16 AND/OR/XOR/NOR, 32 SLT(U),
// 64 DADDU/DSUBU, 128 64-bit shifts, 256 ADDIU/DADDIU, 512 SLTI(U), 1024 ANDI/ORI/XORI, 2048 LUI, 4096 loads,
// 8192 stores.
static u32 op_class(u32 op) {
    u32 p = op >> 26, f = op & 63;
    if (p == 0) {
        if (f <= 3) return 1;
        if (f >= 0x10 && f <= 0x13) return 2;
        if (f == 0x18 || f == 0x19) return 4;
        if (f == 0x21 || f == 0x23) return 8;
        if (f >= 0x24 && f <= 0x27) return 16;
        if (f == 0x2A || f == 0x2B) return 32;
        if (f == 0x2D || f == 0x2F) return 64;
        if (f >= 0x38) return 128;
        return 0;
    }
    if (p == 0x09 || p == 0x19) return 256;
    if (p == 0x0A || p == 0x0B) return 512;
    if (p >= 0x0C && p <= 0x0E) return 1024;
    if (p == 0x0F) return 2048;
    if (p == 0x23 || p == 0x27) return 16384;
    if (p >= 0x20 && p <= 0x27) return 4096;
    if (p == 0x37) return 32768;
    if (p >= 0x28 && p <= 0x2B) return 8192;
    if (p == 0x3F) return 8192;
    return 0;
}

// One non-branch instruction.
static void emit_insn(Ctx* x, u32 op, u32 i, bool inDelay) {
    Emit* e = &x->e;
    int rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
    u32 uimm = op & 0xFFFF, simm = (u32)(s32)(s16)(u16)op;
    u32 prim = op >> 26;
    g_jitStats.insns++;
    if (op == 0) return;  // NOP (SLL zero, zero, 0)
    if (UNLIKELY(g_offMask & op_class(op))) { emit_fallback(x, op, i, inDelay); return; }
    if (prim == 0) {
        switch (op & 63) {
        case 0x00: case 0x02:  // SLL / SRL
            if (!rd) return;
            ldg(e, R0, rt, 0);
            if (sa) mov_shift(e, 0, R0, R0, (op & 63) ? LSR : LSL, sa);
            st32(e, rd, R0, R1);
            return;
        case 0x03:  // SRA: the whole 64-bit register shifts, the low word is kept (vr4300.c)
            if (!rd) return;
            ldg(e, R0, rt, 0);
            if (sa) {
                ldg(e, R1, rt, 1);
                mov_shift(e, 0, R0, R0, LSR, sa);
                dp_reg_c(e, C_AL, ORR, 0, R0, R0, R1, LSL, 32 - sa);
            }
            st32(e, rd, R0, R1);
            return;
        case 0x0F: return;  // SYNC
        case 0x10: case 0x12:  // MFHI / MFLO
            if (!rd) return;
            ldr_imm(e, R0, R11, (op & 2) ? OFF_LO(0) : OFF_HI(0));
            ldr_imm(e, R1, R11, (op & 2) ? OFF_LO(1) : OFF_HI(1));
            st64(e, rd, R0, R1);
            return;
        case 0x11: case 0x13:  // MTHI / MTLO
            ldg(e, R0, rs, 0);
            ldg(e, R1, rs, 1);
            str_imm(e, R0, R11, (op & 2) ? OFF_LO(0) : OFF_HI(0));
            str_imm(e, R1, R11, (op & 2) ? OFF_LO(1) : OFF_HI(1));
            return;
        case 0x18: case 0x19:  // MULT / MULTU: each word of the 64-bit product sign-extended into LO / HI
            ldg(e, R0, rs, 0);
            ldg(e, R1, rt, 0);
            if ((op & 63) == 0x18) smull(e, R2, R3, R0, R1);
            else umull(e, R2, R3, R0, R1);
            str_imm(e, R2, R11, OFF_LO(0));
            mov_shift(e, 0, R0, R2, ASR, 31);
            str_imm(e, R0, R11, OFF_LO(1));
            str_imm(e, R3, R11, OFF_HI(0));
            mov_shift(e, 0, R0, R3, ASR, 31);
            str_imm(e, R0, R11, OFF_HI(1));
            return;
        case 0x21: case 0x23:  // ADDU / SUBU
            if (!rd) return;
            ldg(e, R0, rs, 0);
            ldg(e, R1, rt, 0);
            dp_reg(e, (op & 2) ? SUB : ADD, 0, R0, R0, R1);
            if (UNLIKELY(g_plant == 6)) dp_imm(e, ADD, 0, R0, R0, 1);  // planted bug: the JIT check must catch it
            st32(e, rd, R0, R1);
            return;
        case 0x24: case 0x25: case 0x26: case 0x27: {  // AND / OR / XOR / NOR
            if (!rd) return;
            static const int opc[4] = {AND, ORR, EOR, ORR};
            int o = opc[op & 3];
            ldg(e, R0, rs, 0); ldg(e, R1, rs, 1);
            ldg(e, R2, rt, 0); ldg(e, R3, rt, 1);
            dp_reg(e, o, 0, R0, R0, R2);
            dp_reg(e, o, 0, R1, R1, R3);
            if ((op & 63) == 0x27) { dp_reg(e, MVN, 0, R0, 0, R0); dp_reg(e, MVN, 0, R1, 0, R1); }
            st64(e, rd, R0, R1);
            return;
        }
        case 0x2A: case 0x2B:  // SLT / SLTU (64-bit compare)
            if (!rd) return;
            ldg(e, R0, rs, 0); ldg(e, R1, rs, 1);
            ldg(e, R2, rt, 0); ldg(e, R3, rt, 1);
            dp_reg(e, CMP, 1, 0, R0, R2);
            dp_reg(e, SBC, 1, R12, R1, R3);
            load_const(e, R0, 0);
            dp_imm_c(e, (op & 1) ? C_CC : C_LT, MOV, 0, R0, 0, 1);
            str_imm(e, R0, R11, OFF_GPR(rd, 0));
            load_const(e, R1, 0);
            str_imm(e, R1, R11, OFF_GPR(rd, 1));
            return;
        case 0x2D: case 0x2F:  // DADDU / DSUBU
            if (!rd) return;
            ldg(e, R0, rs, 0); ldg(e, R1, rs, 1);
            ldg(e, R2, rt, 0); ldg(e, R3, rt, 1);
            if ((op & 63) == 0x2D) { dp_reg(e, ADD, 1, R0, R0, R2); dp_reg(e, ADC, 0, R1, R1, R3); }
            else { dp_reg(e, SUB, 1, R0, R0, R2); dp_reg(e, SBC, 0, R1, R1, R3); }
            st64(e, rd, R0, R1);
            return;
        case 0x38: case 0x3A: case 0x3B:  // DSLL / DSRL / DSRA by sa
            if (!rd) return;
            ldg(e, R0, rt, 0); ldg(e, R1, rt, 1);
            if (sa) {
                if ((op & 63) == 0x38) {
                    mov_shift(e, 0, R1, R1, LSL, sa);
                    dp_reg_c(e, C_AL, ORR, 0, R1, R1, R0, LSR, 32 - sa);
                    mov_shift(e, 0, R0, R0, LSL, sa);
                } else {
                    mov_shift(e, 0, R0, R0, LSR, sa);
                    dp_reg_c(e, C_AL, ORR, 0, R0, R0, R1, LSL, 32 - sa);
                    mov_shift(e, 0, R1, R1, (op & 1) ? ASR : LSR, sa);
                }
            }
            st64(e, rd, R0, R1);
            return;
        case 0x3C: case 0x3E: case 0x3F:  // DSLL32 / DSRL32 / DSRA32
            if (!rd) return;
            if ((op & 63) == 0x3C) {
                ldg(e, R1, rt, 0);
                if (sa) mov_shift(e, 0, R1, R1, LSL, sa);
                load_const(e, R0, 0);
            } else {
                ldg(e, R0, rt, 1);
                if ((op & 63) == 0x3F) {
                    mov_shift(e, 0, R1, R0, ASR, 31);
                    if (sa) mov_shift(e, 0, R0, R0, ASR, sa);
                } else {
                    if (sa) mov_shift(e, 0, R0, R0, LSR, sa);
                    load_const(e, R1, 0);
                }
            }
            st64(e, rd, R0, R1);
            return;
        }
        emit_fallback(x, op, i, inDelay);
        return;
    }
    switch (prim) {
    case 0x09:  // ADDIU
        if (!rt) return;
        ldg(e, R0, rs, 0);
        imm_op(e, ADD, 0, R0, R0, simm, R2);
        st32(e, rt, R0, R1);
        return;
    case 0x19:  // DADDIU
        if (!rt) return;
        ldg(e, R0, rs, 0); ldg(e, R1, rs, 1);
        imm_op(e, ADD, 1, R0, R0, simm, R2);
        if (simm & 0x80000000u) dp_imm(e, SBC, 0, R1, R1, 0);  // + 0xFFFFFFFF + carry
        else dp_imm(e, ADC, 0, R1, R1, 0);
        st64(e, rt, R0, R1);
        return;
    case 0x0A: case 0x0B:  // SLTI / SLTIU
        if (!rt) return;
        ldg(e, R0, rs, 0); ldg(e, R1, rs, 1);
        load_const(e, R2, simm);
        load_const(e, R3, sx_hi(simm));
        dp_reg(e, CMP, 1, 0, R0, R2);
        dp_reg(e, SBC, 1, R12, R1, R3);
        load_const(e, R0, 0);
        dp_imm_c(e, (prim & 1) ? C_CC : C_LT, MOV, 0, R0, 0, 1);
        str_imm(e, R0, R11, OFF_GPR(rt, 0));
        load_const(e, R1, 0);
        str_imm(e, R1, R11, OFF_GPR(rt, 1));
        return;
    case 0x0C:  // ANDI
        if (!rt) return;
        ldg(e, R0, rs, 0);
        imm_op(e, AND, 0, R0, R0, uimm, R2);
        load_const(e, R1, 0);
        st64(e, rt, R0, R1);
        return;
    case 0x0D: case 0x0E:  // ORI / XORI
        if (!rt) return;
        ldg(e, R0, rs, 0); ldg(e, R1, rs, 1);
        imm_op(e, prim == 0x0D ? ORR : EOR, 0, R0, R0, uimm, R2);
        st64(e, rt, R0, R1);
        return;
    case 0x0F:  // LUI
        if (!rt) return;
        load_const(e, R0, uimm << 16);
        load_const(e, R1, sx_hi(uimm << 16));
        st64(e, rt, R0, R1);
        return;
    case 0x2F: return;  // CACHE: no cache model; blocks run in kernel mode only
    case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x27: case 0x37:
    case 0x28: case 0x29: case 0x2B: case 0x3F:
        emit_mem(x, op, i, inDelay);
        return;
    }
    emit_fallback(x, op, i, inDelay);
}

// A branch at index i, its delay slot, and the block's exits.
static void emit_branch(Ctx* x, u32 op, u32 i, u32 slotOp) {
    Emit* e = &x->e;
    u32 va = x->va0 + 4 * i;
    u32 prim = op >> 26;
    int rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
    u32 btarget = va + 4 + ((u32)(s32)(s16)(u16)op << 2);
    u32 jtarget = ((va + 4) & 0xF0000000u) | ((op & 0x03FFFFFF) << 2);
    bool likely = false, dynamic = false, always = false;
    u32 target = btarget;
    int cond = C_AL;
    int link = -1;  // guest register receiving va + 8
    g_jitStats.insns++;
    if (prim == 0 && (op & 63) == 8) { dynamic = true; always = true; }                 // JR
    else if (prim == 0) { dynamic = true; always = true; link = rd; }                   // JALR
    else if (prim == 2) { always = true; target = jtarget; }                            // J
    else if (prim == 3) { always = true; target = jtarget; link = 31; }                 // JAL
    else if (prim == 1) {                                                                // REGIMM
        ldg(e, R1, rs, 1);
        dp_imm(e, CMP, 1, 0, R1, 0);
        cond = (rt & 1) ? C_GE : C_LT;
        likely = (rt & 2) != 0;
        if (rt & 0x10) link = 31;
    } else {
        likely = prim >= 0x14;
        switch (prim & 3) {
        case 0: case 1:  // BEQ / BNE
            ldg(e, R0, rs, 0); ldg(e, R2, rt, 0);
            ldg(e, R1, rs, 1); ldg(e, R3, rt, 1);
            dp_reg(e, CMP, 1, 0, R0, R2);
            dp_reg_c(e, C_EQ, CMP, 1, 0, R1, R3, LSL, 0);
            cond = (prim & 1) ? C_NE : C_EQ;
            break;
        default:  // BLEZ / BGTZ: flags of 0 - rs (64-bit)
            ldg(e, R0, rs, 0); ldg(e, R1, rs, 1);
            dp_imm(e, RSB, 1, R2, R0, 0);
            dp_imm(e, RSC, 1, R3, R1, 0);
            cond = (prim & 1) ? C_LT : C_GE;
            break;
        }
    }
    if (dynamic) {
        ldg(e, R5, rs, 0);
        ldg(e, R8, rs, 1);
    }
    if (!always) {
        load_const(e, R4, 0);
        dp_imm_c(e, cond, MOV, 0, R4, 0, 1);
    }
    if (link >= 0 && link != 0) {
        load_const(e, R0, va + 8);
        load_const(e, R1, sx_hi(va + 8));
        st64(e, link, R0, R1);
    }
    u32 skip = 0;
    if (likely) {
        dp_imm(e, CMP, 1, 0, R4, 0);
        skip = branch_c(e, C_EQ, 0);
    }
    emit_insn(x, slotOp, i + 1, true);
    if (dynamic) {
        emit_exit_reg(x, i + 2);
    } else if (always || likely) {
        emit_exit_const(x, target, i + 2);
    } else {
        dp_imm(e, CMP, 1, 0, R4, 0);
        u32 nt = branch_c(e, C_EQ, 0);
        emit_exit_const(x, target, i + 2);
        patch_branch(e, nt, e->n);
        emit_exit_const(x, va + 8, i + 2);
    }
    if (likely) {
        patch_branch(e, skip, e->n);
        emit_exit_const(x, va + 8, i + 1);  // not taken: the delay slot does not run
    }
}

// ---- Compiling a block ---------------------------------------------------------------------------------------------
static Block* compile(u32 va, u32 pa) {
    u32 n = 0, br = MAX_INSNS;
    // A block stays inside its 4 KB page: at a TLB-mapped address the next virtual page can map anywhere.
    u32 room = (0x1000 - (pa & 0xFFF)) / 4;
    if (room > MAX_INSNS) room = MAX_INSNS;
    for (u32 i = 0; i < room; i++) {
        if (pa + 4 * i + 4 > g_mem.rdramSize) break;
        u32 op = m_r32(g_mem.rdram, pa + 4 * i);
        if (is_branch(op)) {
            // BC1 needs the coprocessor-usable check; a branch in a delay slot is undefined: interpreter for both
            bool ok = (op >> 26) != 0x11 && i + 1 < room && pa + 4 * i + 8 <= g_mem.rdramSize &&
                      !is_branch(m_r32(g_mem.rdram, pa + 4 * i + 4));
            if (ok) { br = i; n = i + 2; }
            break;
        }
        n = i + 1;
        if ((op >> 26) == 0x10) break;  // COP0 ends the block (vr_jit_fallback leaves after it)
    }
    if (!n) return &g_interp;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (g_nblocks >= MAX_BLOCKS || g_codeCap - g_codeUsed < 16384) jit_flush();
        Ctx x;
        memset(&x, 0, sizeof x);
        x.e.buf = g_code + g_codeUsed;
        x.e.cap = g_codeCap - g_codeUsed;
        x.va0 = va;
        x.n = n;
        Emit* e = &x.e;
        push_regs(e, (1u << R4) | (1u << R5) | (1u << R6) | (1u << R7) | (1u << R8) | (1u << R9) | (1u << R10) |
                         (1u << R11) | (1u << LR));
        dp_imm(e, SUB, 0, SP, SP, 4);  // 8-byte stack alignment for calls
        mov_reg(e, R11, R0);
        ldr_imm(e, R6, R11, OFF_CYC(0));
        ldr_imm(e, R7, R11, OFF_CYC(1));
        load_const(e, R10, (u32)(uintptr_t)g_mem.rdram);
        load_const(e, R9, (u32)(uintptr_t)g_codePage);
        u32 end = br < MAX_INSNS ? br : n;
        for (u32 i = 0; i < end; i++) emit_insn(&x, m_r32(g_mem.rdram, pa + 4 * i), i, false);
        if (br < MAX_INSNS) emit_branch(&x, m_r32(g_mem.rdram, pa + 4 * br), br, m_r32(g_mem.rdram, pa + 4 * br + 4));
        else emit_exit_const(&x, va + 4 * n, n);
        for (u32 k = 0; k < x.nexits; k++) patch_branch(e, x.exits[k], e->n);
        epilogue(e);  // the shared return: the fallback set pc, npc and cycles
        if (e->overflow) { jit_flush(); continue; }
        jit_host_sync_icache(x.e.buf, e->n * 4);
        Block* b = &g_blocks[g_nblocks++];
        b->code = (void (*)(Vr*))(uintptr_t)x.e.buf;
        b->n = n;
        b->va = va;
        b->idle = false;
        if (n == 2 && br == 0 && m_r32(g_mem.rdram, pa + 4) == 0) {  // `b .` / `j .` with a NOP: an idle loop
            u32 op = m_r32(g_mem.rdram, pa), prim = op >> 26;
            bool selfB = prim == 4 && ((op >> 21) & 31) == ((op >> 16) & 31) && (op & 0xFFFF) == 0xFFFF;
            bool selfJ = prim == 2 && ((((va + 4) & 0xF0000000u) | ((op & 0x03FFFFFF) << 2)) == va);
            b->idle = selfB || selfJ;
        }
        g_codeUsed += e->n;
        g_codePage[pa >> 12] = 1;
        g_codePage[(pa + 4 * n - 1) >> 12] = 1;
        g_jitStats.compiles++;
        return b;
    }
    return &g_interp;
}

static inline bool kernel_mode(const Vr* c) {
    u32 s = (u32)c->cop0[C0_STATUS];
    return (s & (ST_EXL | ST_ERL)) || ((s >> 3) & 3) == 0;
}

void jit_run(Vr* c, u64 untilCycles) {
    g_vrRunUntil = untilCycles;
    while (c->cycles < g_vrRunUntil && !g_cpuStop) {
        u32 a = (u32)c->pc;
        u32 pa = a & 0x1FFFFFFF;
        bool ok = !g_vrIrqPending && !c->nextInDelay && !c->branchPending && c->pc == SEXT32(a) && !(a & 3) &&
                  kernel_mode(c);
        if (ok && (a >> 30) != 2) ok = vr_probe_fetch(c, c->pc, &pa);  // TLB-mapped code (GoldenEye's at 0x7F...)
        if (!ok || pa >= g_mem.rdramSize) {
            vr_step(c);
            g_jitStats.steps++;
            continue;
        }
        Block** page = g_map[pa >> 12];
        if (!page) page = g_map[pa >> 12] = calloc(1024, sizeof(Block*));
        Block* b = page ? page[(pa & 0xFFF) >> 2] : NULL;
        if (b && b->code && b->va != a) b = NULL;  // the page is mapped at another address now: compile for this one
        if (b && b->idle) {
            // The interpreter would run the loop until the event; registers do not change, only time does.
            u64 iters = (g_vrRunUntil - c->cycles) / 2;
            if (iters) {
                c->cycles += 2 * iters;
                g_jitStats.idleCycles += 2 * iters;
                continue;
            }
        }
        if (!b) {
            if (g_vrRunUntil - c->cycles < MAX_INSNS) {  // near an event: step rather than compile a fragment
                vr_step(c);
                g_jitStats.steps++;
                continue;
            }
            b = compile(a, pa);
            if (page) page[(pa & 0xFFF) >> 2] = b;
        }
        if (!b->code || c->cycles + b->n > g_vrRunUntil) {
            vr_step(c);
            g_jitStats.steps++;
            continue;
        }
        g_jitStats.entries++;
        b->code(c);
    }
}

#endif
