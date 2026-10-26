// The high-level graphics microcode on core 2 (job option --async on): a graphics task's display list runs here while
// core 0 goes on emulating the CPU, which waits for it only at the task's emulated end (src/sys/sp.c). Everything that
// drives the GPU (rdp_pica.c) runs on this thread, presents included: the main thread queues them in order with the
// tasks. Jobs are taken one at a time from a small ring.
#include <3ds.h>
#include <string.h>
#include "hle_worker.h"
#include "rdp_pica.h"
#include "../src/sys/sys.h"
#include "../src/hle/hle.h"

enum { JOB_TASK, JOB_PRESENT };
typedef struct { int kind; u32 origin, width, height; } Job;

#define RING 8
static Job g_ring[RING];
static volatile u32 g_head, g_tail;   // the main thread writes head, the worker tail
static LightEvent g_work, g_idle;
static Thread g_thread;
static volatile bool g_stop;
static volatile bool g_presentShown = true;  // the last present found a target (else the main thread shows RDRAM)
WorkerStats g_workerStats;

static void worker(void* arg) {
    (void)arg;
    for (;;) {
        while (g_tail == g_head && !g_stop) LightEvent_Wait(&g_work);
        if (g_stop && g_tail == g_head) break;
        Job j = g_ring[g_tail % RING];
        u64 t0 = svcGetSystemTick();
        if (j.kind == JOB_TASK) hle_gfx_run();
        else g_presentShown = rdp_pica_present(j.origin, j.width, j.height);
        g_workerStats.busyTicks += svcGetSystemTick() - t0;
        __dmb();
        g_tail = g_tail + 1;
        LightEvent_Signal(&g_idle);
    }
}

static void push(const Job* j) {
    while (g_head - g_tail >= RING) {  // full: wait for the worker
        u64 t0 = svcGetSystemTick();
        LightEvent_Wait(&g_idle);
        g_workerStats.waitTicks += svcGetSystemTick() - t0;
    }
    g_ring[g_head % RING] = *j;
    __dmb();
    g_head = g_head + 1;
    LightEvent_Signal(&g_work);
}

static void submit(void) {
    Job j = {JOB_TASK, 0, 0, 0};
    push(&j);
}

static void wait_idle(void) {
    if (g_tail == g_head) return;
    u64 t0 = svcGetSystemTick();
    while (g_tail != g_head) LightEvent_Wait(&g_idle);
    g_workerStats.waitTicks += svcGetSystemTick() - t0;
}

static const HleAsync kAsync = {submit, wait_idle};

bool worker_start(void) {
    if (g_thread) return true;
    LightEvent_Init(&g_work, RESET_ONESHOT);
    LightEvent_Init(&g_idle, RESET_ONESHOT);
    g_stop = false;
    g_head = g_tail = 0;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_thread = threadCreate(worker, NULL, 64 * 1024, prio, 2, false);
    return g_thread != NULL;
}

void worker_attach(bool on) {
    wait_idle();
    g_hleAsync = on && g_thread ? &kAsync : NULL;
}

void worker_present(u32 origin, u32 width, u32 height) {
    Job j = {JOB_PRESENT, origin, width, height};
    push(&j);
}

bool worker_last_present_shown(void) { return g_presentShown; }
void worker_wait(void) { wait_idle(); }
