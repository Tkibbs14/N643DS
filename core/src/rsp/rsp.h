#pragma once
// RSP (the RCP's signal processor) interpreter: scalar unit, vector unit, its COP0 (the SP / DP registers). The
// low-level reference that high-level microcode will be checked against (docs/DESIGN.md "RSP plan"). Behaviour follows
// the RSP documentation on n64brew and ares; PeterLemon's RSPTest ROMs (hardware captures) are the check.
#include "../common.h"

typedef struct {
    u32 r[32];          // scalar registers (r[0] = 0)
    u32 pc;             // IMEM offset (12 bits)
    u32 npc;
    bool inDelay, branchPending;
    u32 branchTo;
    u16 v[32][8];       // vector registers, lane 0 = bytes 0-1 (big-endian element order)
    s64 acc[8];         // 48-bit accumulators, sign-extended
    u16 vco, vcc;       // carry / not-equal (VCO), compare (VCC): low byte = lanes' low bits, high byte = high bits
    u8 vce;
    s16 divIn, divOut;
    bool divDp;         // a VRCPH / VRSQH set the high half of the next input
    u64 cycles;         // RSP cycles run (statistics, task timing)
    bool halted;        // a BREAK ran (or the CPU halted it)
} Rsp;

extern Rsp g_rsp;

void rsp_reset(void);
// Runs from the current pc until a BREAK or `maxCycles` (a microcode that never ends). Returns the cycles run.
u64 rsp_run(u64 maxCycles);
void rsp_init_tables(void);
