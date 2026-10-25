// RSP scalar unit: a 32-bit MIPS subset running from IMEM (4 KB, 12-bit pc), loads and stores in DMEM (4 KB, any
// alignment, wrapping), COP0 = the SP and DP registers, COP2 = the vector unit (rsp_vu.c). No exceptions; BREAK halts.
#include "rsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sys/sys.h"

Rsp g_rsp;
u64 g_rspTrace;

void rsp_vu_cop2(u32 op);
void rsp_vu_lwc2(u32 op);
void rsp_vu_swc2(u32 op);

#define RS ((op >> 21) & 31)
#define RT ((op >> 16) & 31)
#define RD ((op >> 11) & 31)
#define SA ((op >> 6) & 31)
#define SIMM ((u32)(s32)(s16)(u16)op)

static inline u8* dmem(void) { return g_mem.spmem; }
static inline u8 d_r8(u32 a) { return m_r8(g_mem.spmem, a & 0xFFF); }
static inline void d_w8(u32 a, u8 v) { m_w8(g_mem.spmem, a & 0xFFF, v); }
static u32 d_r16(u32 a) { return ((u32)d_r8(a) << 8) | d_r8(a + 1); }
static u32 d_r32(u32 a) { return ((u32)d_r8(a) << 24) | ((u32)d_r8(a + 1) << 16) | ((u32)d_r8(a + 2) << 8) | d_r8(a + 3); }
static void d_w16(u32 a, u32 v) { d_w8(a, (u8)(v >> 8)); d_w8(a + 1, (u8)v); }
static void d_w32(u32 a, u32 v) { d_w8(a, (u8)(v >> 24)); d_w8(a + 1, (u8)(v >> 16)); d_w8(a + 2, (u8)(v >> 8)); d_w8(a + 3, (u8)v); }

// COP0: registers 0-7 are SP_*, 8-15 are DP_* (the same as the CPU sees them).
static u32 cop0_read(int r) {
    r &= 15;
    return r < 8 ? sp_read((u32)r * 4) : dp_read((u32)(r - 8) * 4);
}
static void cop0_write(int r, u32 v) {
    r &= 15;
    if (r < 8) sp_write((u32)r * 4, v, 0xFFFFFFFFu);
    else dp_write((u32)(r - 8) * 4, v, 0xFFFFFFFFu);
}

static inline void branch(bool taken, u32 target) {
    g_rsp.inDelay = true;  // the next instruction is the delay slot
    if (taken) { g_rsp.branchPending = true; g_rsp.branchTo = target & 0xFFC; }
}

static void step(void) {
    Rsp* c = &g_rsp;
    u32 pc = c->pc;
    u32 op = m_r32(g_mem.spmem, 0x1000 | (pc & 0xFFC));
    bool wasDelay = c->inDelay;
    bool pend = c->branchPending;
    u32 to = c->branchTo;
    c->inDelay = false;
    c->branchPending = false;
    u32* r = c->r;
    u32 rs = r[RS], rt = r[RT];
    if (g_rspTrace) {  // N64_RSP_TRACE=n: the first n instructions (debugging)
        g_rspTrace--;
        fprintf(stderr, "  rsp %03x: %08x  rs=%08x rt=%08x\n", (unsigned)pc, (unsigned)op, (unsigned)rs, (unsigned)rt);
    }
    switch (op >> 26) {
    case 0x00:
        switch (op & 63) {
        case 0x00: r[RD] = rt << SA; break;                         // SLL
        case 0x02: r[RD] = rt >> SA; break;                         // SRL
        case 0x03: r[RD] = (u32)((s32)rt >> SA); break;             // SRA
        case 0x04: r[RD] = rt << (rs & 31); break;                  // SLLV
        case 0x06: r[RD] = rt >> (rs & 31); break;                  // SRLV
        case 0x07: r[RD] = (u32)((s32)rt >> (rs & 31)); break;      // SRAV
        case 0x08: branch(true, rs); break;                         // JR
        case 0x09: { u32 t = rs; r[RD] = (pc + 8) & 0xFFC; branch(true, t); break; }  // JALR
        case 0x0D:                                                  // BREAK
            c->halted = true;
            break;
        case 0x20: case 0x21: r[RD] = rs + rt; break;               // ADD / ADDU (no overflow trap on the RSP)
        case 0x22: case 0x23: r[RD] = rs - rt; break;               // SUB / SUBU
        case 0x24: r[RD] = rs & rt; break;
        case 0x25: r[RD] = rs | rt; break;
        case 0x26: r[RD] = rs ^ rt; break;
        case 0x27: r[RD] = ~(rs | rt); break;
        case 0x2A: r[RD] = (s32)rs < (s32)rt; break;
        case 0x2B: r[RD] = rs < rt; break;
        default: break;  // reserved: no-op
        }
        break;
    case 0x01: {
        u32 target = pc + 4 + (SIMM << 2);
        bool lt = (s32)rs < 0;
        switch (RT) {
        case 0x00: branch(lt, target); break;                                 // BLTZ
        case 0x01: branch(!lt, target); break;                                // BGEZ
        case 0x10: r[31] = (pc + 8) & 0xFFC; branch(lt, target); break;       // BLTZAL
        case 0x11: r[31] = (pc + 8) & 0xFFC; branch(!lt, target); break;      // BGEZAL
        }
        break;
    }
    case 0x02: branch(true, (op & 0x3FF) << 2); break;                                  // J
    case 0x03: r[31] = (pc + 8) & 0xFFC; branch(true, (op & 0x3FF) << 2); break;       // JAL
    case 0x04: branch(rs == rt, pc + 4 + (SIMM << 2)); break;                          // BEQ
    case 0x05: branch(rs != rt, pc + 4 + (SIMM << 2)); break;                          // BNE
    case 0x06: branch((s32)rs <= 0, pc + 4 + (SIMM << 2)); break;                      // BLEZ
    case 0x07: branch((s32)rs > 0, pc + 4 + (SIMM << 2)); break;                       // BGTZ
    case 0x08: case 0x09: r[RT] = rs + SIMM; break;                                    // ADDI / ADDIU
    case 0x0A: r[RT] = (s32)rs < (s32)SIMM; break;                                     // SLTI
    case 0x0B: r[RT] = rs < SIMM; break;                                               // SLTIU
    case 0x0C: r[RT] = rs & (op & 0xFFFF); break;
    case 0x0D: r[RT] = rs | (op & 0xFFFF); break;
    case 0x0E: r[RT] = rs ^ (op & 0xFFFF); break;
    case 0x0F: r[RT] = (op & 0xFFFF) << 16; break;                                     // LUI
    case 0x10:                                                                         // COP0
        if (RS == 0) r[RT] = cop0_read(RD);
        else if (RS == 4) cop0_write(RD, rt);
        break;
    case 0x12: rsp_vu_cop2(op); break;
    case 0x20: r[RT] = (u32)(s32)(s8)d_r8(rs + SIMM); break;                           // LB
    case 0x21: r[RT] = (u32)(s32)(s16)d_r16(rs + SIMM); break;                         // LH
    case 0x23: case 0x27: r[RT] = d_r32(rs + SIMM); break;                             // LW (LWU)
    case 0x24: r[RT] = d_r8(rs + SIMM); break;                                         // LBU
    case 0x25: r[RT] = d_r16(rs + SIMM); break;                                        // LHU
    case 0x28: d_w8(rs + SIMM, (u8)rt); break;                                         // SB
    case 0x29: d_w16(rs + SIMM, rt); break;                                            // SH
    case 0x2B: d_w32(rs + SIMM, rt); break;                                            // SW
    case 0x32: rsp_vu_lwc2(op); break;
    case 0x3A: rsp_vu_swc2(op); break;
    default: break;
    }
    r[0] = 0;
    c->cycles++;
    // A branch's target applies after its delay slot.
    if (wasDelay && pend) c->pc = to;
    else c->pc = (pc + 4) & 0xFFC;
    (void)dmem;
}

u64 rsp_run(u64 maxCycles) {
    u64 start = g_rsp.cycles;
    g_rsp.halted = false;
    while (!g_rsp.halted && g_rsp.cycles - start < maxCycles) {
        step();
        // A BREAK in a delay slot still lets the branch complete its pc update (the next run continues there).
    }
    return g_rsp.cycles - start;
}

void rsp_reset(void) {
    memset(&g_rsp, 0, sizeof g_rsp);
    rsp_init_tables();
    if (getenv("N64_RSP_TRACE")) g_rspTrace = strtoull(getenv("N64_RSP_TRACE"), NULL, 0);
}

// Accessors the vector unit's loads / stores use (rsp_vu.c)
u8 rsp_dmem_r8(u32 a);
void rsp_dmem_w8(u32 a, u8 v);
u8 rsp_dmem_r8(u32 a) { return d_r8(a); }
void rsp_dmem_w8(u32 a, u8 v) { d_w8(a, v); }
