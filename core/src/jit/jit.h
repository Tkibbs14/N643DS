#pragma once
// VR4300 -> ARMv6K JIT (docs/DESIGN.md "JIT"). Exactly equivalent to the interpreter: a block runs only when all of
// it fits before the scheduler's next event, an instruction it does not translate runs through the interpreter
// (vr_jit_fallback), and a block ends after any call that may raise an interrupt or move the next event. A whole run
// with and without the JIT must retire the same instructions and end in the same state (core/tools/jit_check.sh).
#include "../common.h"

struct Vr;
bool jit_available(void);       // compiled for ARM
bool jit_init(void);            // allocates the code buffer; false: no JIT (the interpreter runs)
void jit_run(struct Vr* c, u64 untilCycles);
void jit_flush(void);           // drops all compiled code
extern bool g_jitEnabled;

typedef struct {
    u64 blocks, compiles, flushes, invalidations, insns, fallbacks;  // fallbacks: instructions not translated
    u64 entries, steps;  // block entries, interpreter steps (budget, delay slots, unmapped code)
    u64 idleCycles;      // cycles fast-forwarded through idle loops
} JitStats;
extern JitStats g_jitStats;

// Host services (src/host/jit_host_linux.c for qemu, core/host3ds/jit_3ds.c for the 3DS)
void* jit_host_alloc_exec(u32 bytes);
void jit_host_sync_icache(void* start, u32 bytes);
