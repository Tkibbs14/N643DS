// Where core 0's time goes (job option --profile on): a thread on core 1 samples g_phase (what the emulator is doing:
// dispatcher, compiled block, interpreter, HLE graphics, GPU feed, other) and the guest address it is at every ~100 us,
// and logs the shares and the hottest guest blocks at the end of the run. The phase markers cost a store at each
// transition (src/sys/sys.h g_phase).
#include <3ds.h>
#include <string.h>
#include "profile.h"
#include "../src/sys/sys.h"
#include "../src/cpu/vr4300.h"

#define SLOTS 4096
static struct { u32 va; u32 n; } g_hot[2][SLOTS];  // [0]: compiled blocks by entry address, [1]: interpreter by pc
static u32 g_count[PH_N];
static volatile bool g_run;
static Thread g_thread;

static void note(int k, u32 va) {
    u32 h = (va >> 2) * 2654435761u >> 20;  // 12 bits
    for (u32 i = 0; i < 16; i++) {
        u32 s = (h + i) & (SLOTS - 1);
        if (g_hot[k][s].va == va) { g_hot[k][s].n++; return; }
        if (!g_hot[k][s].n) { g_hot[k][s].va = va; g_hot[k][s].n = 1; return; }
    }
}

static void sampler(void* arg) {
    (void)arg;
    while (g_run) {
        u32 ph = g_phase;
        if (ph < PH_N) g_count[ph]++;
        if (ph == PH_BLOCK) note(0, g_phaseVa);
        else if (ph == PH_INTERP) note(1, (u32)g_cpu.pc);
        svcSleepThread(100000);
    }
}

void profile_start(void) {
    memset(g_hot, 0, sizeof g_hot);
    memset(g_count, 0, sizeof g_count);
    g_run = true;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_thread = threadCreate(sampler, NULL, 4096, prio - 1 < 0x18 ? 0x18 : prio - 1, 1, false);
    if (!g_thread) g_run = false;
}

void profile_stop(void (*log)(const char* fmt, ...)) {
    if (!g_thread) return;
    g_run = false;
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    g_thread = NULL;
    static const char* names[PH_N] = {"other", "dispatcher", "compiled", "interpreter", "hle graphics", "gpu"};
    u32 total = 0;
    for (int i = 0; i < PH_N; i++) total += g_count[i];
    if (!total) return;
    log("# profile: %lu samples:", (unsigned long)total);
    for (int i = 0; i < PH_N; i++) log(" %s %.1f%%", names[i], 100.0 * g_count[i] / total);
    log("\n");
    for (int k = 0; k < 2; k++) {
        log(k ? "# profile: hottest interpreter pcs:" : "# profile: hottest compiled blocks:");
        for (int top = 0; top < 16; top++) {
            int best = -1;
            for (int s = 0; s < SLOTS; s++)
                if (g_hot[k][s].n && (best < 0 || g_hot[k][s].n > g_hot[k][best].n)) best = s;
            if (best < 0) break;
            log(" %08lx %.1f%%;", (unsigned long)g_hot[k][best].va, 100.0 * g_hot[k][best].n / total);
            g_hot[k][best].n = 0;
        }
        log("\n");
    }
}
