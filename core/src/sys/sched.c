// Event scheduler: a handful of fixed events, each pending or not, scanned for the earliest. Few enough that a scan
// beats a heap.
#include "sys.h"
#include "../cpu/vr4300.h"

static u64 g_when[EV_N];
static bool g_on[EV_N];

void vr_run_limit(u64 when);
void sched_compare_event(void);

void sched_reset(void) {
    for (int i = 0; i < EV_N; i++) g_on[i] = false;
}

void sched_at(int ev, u64 when) {
    g_when[ev] = when;
    g_on[ev] = true;
    vr_run_limit(when);
}

void sched_cancel(int ev) { g_on[ev] = false; }

u64 sched_next(void) {
    u64 t = UINT64_MAX;
    for (int i = 0; i < EV_N; i++)
        if (g_on[i] && g_when[i] < t) t = g_when[i];
    return t;
}

void sched_dispatch(u64 now) {
    for (;;) {
        int best = -1;
        for (int i = 0; i < EV_N; i++)
            if (g_on[i] && g_when[i] <= now && (best < 0 || g_when[i] < g_when[best])) best = i;
        if (best < 0) return;
        g_on[best] = false;
        switch (best) {
        case EV_COMPARE: sched_compare_event(); break;
        case EV_VI_INTR: vi_intr_event(); break;
        case EV_VI_FIELD: vi_field_event(); break;
        case EV_PI_DMA: pi_dma_done(); break;
        case EV_SI_DMA: si_dma_done(); break;
        case EV_AI_BUF: ai_buf_event(); break;
        case EV_SP_TASK: sp_task_done(); break;
        case EV_DP_DONE: dp_done_event(); break;
        }
    }
}
