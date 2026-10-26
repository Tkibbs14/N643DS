#pragma once
// The high-level graphics microcode and the GPU on core 2 (hle_worker.c).
#include "../src/common.h"

bool worker_start(void);            // the thread (false: core 2 not available; everything stays on core 0)
void worker_attach(bool on);        // graphics tasks through the worker (g_hleAsync) or synchronously
void worker_present(u32 origin, u32 width, u32 height);  // queued after the tasks before it
bool worker_last_present_shown(void);  // the last present found a GPU target (else show RDRAM)
void worker_wait(void);             // until the worker has nothing queued (before touching the GPU elsewhere)

typedef struct { u64 busyTicks, waitTicks; } WorkerStats;  // the worker's working time; core 0 waiting for it
extern WorkerStats g_workerStats;
