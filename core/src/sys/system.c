// Power-on and the main loop: the CPU runs to the next scheduled event, then the event fires.
#include <stdlib.h>
#include <string.h>
#include "sys.h"
#include "system.h"
#include "../cpu/vr4300.h"
#include "../jit/jit.h"
#include "../rsp/rsp.h"
void rdp_soft_reset(void);

void mi_reset(void);
volatile bool g_quit;

bool sys_init(u32 rdramSize, int saveType) {
    free(g_mem.rdram);
    g_mem.rdram = calloc(1, RDRAM_MAX);
    if (!g_mem.rdram) return false;
    g_mem.rdramSize = rdramSize;
    free(g_mem.save);
    static const u32 sizes[] = {0, 512, 2048, 32768, 98304, 131072};
    g_mem.saveType = saveType;
    g_mem.saveSize = sizes[saveType];
    g_mem.save = g_mem.saveSize ? calloc(1, g_mem.saveSize) : NULL;
    if (saveType == SAVE_FLASH128K && g_mem.save) memset(g_mem.save, 0xFF, g_mem.saveSize);
    return true;
}

void sys_reset(void) {
    // Compiled code belongs to the cartridge that was running (the 3DS host runs several in one process: the CPU
    // tests ran the previous test's blocks, relay job 1443).
    jit_flush();
    memset(&g_jitStats, 0, sizeof g_jitStats);
    memset(g_mem.spmem, 0, sizeof g_mem.spmem);
    memset(g_mem.isv, 0, sizeof g_mem.isv);
    memset(&g_stats, 0, sizeof g_stats);
    sched_reset();
    rsp_reset();
    rdp_reset();
    rdp_soft_reset();
    vr_reset(&g_cpu);
    mi_reset();
    vi_reset();
    ai_reset();
    pi_reset();
    si_reset();
    sp_reset();
    dp_reset();
}

void sys_run(u64 untilCycles) {
    while (!g_quit && g_cpu.cycles < untilCycles) {
        u64 next = sched_next();
        if (next > untilCycles) next = untilCycles;
        vr_run(&g_cpu, next);
        if (g_cpuStop) break;
        sched_dispatch(g_cpu.cycles);
    }
}
