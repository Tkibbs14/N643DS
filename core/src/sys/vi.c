// Video Interface: registers, the half-line counter and the field timing. The field length follows the registers
// (H_SYNC VI clocks a line, V_SYNC + 1 half-lines a field), so PAL, NTSC and interlaced modes time themselves.
#include "sys.h"
#include "../cpu/vr4300.h"

ViState g_vi;

static u32 halflines(void) { return (g_vi.vSync & 0x3FF) ? (g_vi.vSync & 0x3FF) + 1 : 526; }

static u32 field_cycles(void) {
    u32 hs = g_vi.hSync & 0xFFF;
    if (!hs || !(g_vi.vSync & 0x3FF)) return CPU_HZ / 60;
    u64 clock = g_mem.pal ? VI_CLOCK_PAL : VI_CLOCK_NTSC;
    return (u32)((u64)CPU_HZ * (hs + 1) * halflines() / (2 * clock));
}

static void schedule_intr(void) {
    u32 hl = halflines();
    u32 target = g_vi.vIntr & 0x3FF;
    if (target >= hl) { sched_cancel(EV_VI_INTR); return; }
    u64 at = g_vi.fieldStart + (u64)target * g_vi.cyclesPerField / hl;
    if (at < g_cpu.cycles) { sched_cancel(EV_VI_INTR); return; }  // already passed this field
    sched_at(EV_VI_INTR, at);
}

static u32 v_current(void) {
    u64 into = g_cpu.cycles - g_vi.fieldStart;
    u32 hl = (u32)(into * halflines() / g_vi.cyclesPerField);
    if (hl >= halflines()) hl = halflines() - 1;
    bool interlaced = (g_vi.ctrl >> 6) & 1;
    return (hl & ~1u) | (interlaced ? g_vi.field : 0);
}

void vi_reset(void) {
    ViState z = {0};
    g_vi = z;
    g_vi.cyclesPerField = CPU_HZ / 60;
    g_vi.fieldStart = g_cpu.cycles;
    sched_at(EV_VI_FIELD, g_vi.fieldStart + g_vi.cyclesPerField);
}

void vi_field_event(void) {
    g_stats.fields++;
    if (g_onField) g_onField();
    g_vi.fieldStart += g_vi.cyclesPerField;
    if ((g_vi.ctrl >> 6) & 1) g_vi.field ^= 1;
    else g_vi.field = 0;
    g_vi.cyclesPerField = field_cycles();
    sched_at(EV_VI_FIELD, g_vi.fieldStart + g_vi.cyclesPerField);
    schedule_intr();
}

void vi_intr_event(void) { mi_raise(MI_VI); }

u32 vi_read(u32 reg) {
    switch (reg) {
    case 0x00: return g_vi.ctrl;
    case 0x04: return g_vi.origin;
    case 0x08: return g_vi.width;
    case 0x0C: return g_vi.vIntr;
    case 0x10: return v_current();
    case 0x14: return g_vi.burst;
    case 0x18: return g_vi.vSync;
    case 0x1C: return g_vi.hSync;
    case 0x20: return g_vi.leap;
    case 0x24: return g_vi.hStart;
    case 0x28: return g_vi.vStart;
    case 0x2C: return g_vi.vBurst;
    case 0x30: return g_vi.xScale;
    case 0x34: return g_vi.yScale;
    }
    return 0;
}

void vi_write(u32 reg, u32 v, u32 mask) {
    (void)mask;
    switch (reg) {
    case 0x00: g_vi.ctrl = v & 0x1FBFF; break;
    case 0x04: g_vi.origin = v & 0xFFFFFF; break;
    case 0x08: g_vi.width = v & 0xFFF; break;
    case 0x0C: g_vi.vIntr = v & 0x3FF; schedule_intr(); break;
    case 0x10: mi_clear(MI_VI); break;
    case 0x14: g_vi.burst = v & 0x3FFFFFFF; break;
    case 0x18: g_vi.vSync = v & 0x3FF; break;
    case 0x1C: g_vi.hSync = v & 0x1FFFFF; break;
    case 0x20: g_vi.leap = v & 0xFFFFFFF; break;
    case 0x24: g_vi.hStart = v & 0x3FF03FF; break;
    case 0x28: g_vi.vStart = v & 0x3FF03FF; break;
    case 0x2C: g_vi.vBurst = v & 0x3FF03FF; break;
    case 0x30: g_vi.xScale = v & 0xFFF0FFF; break;
    case 0x34: g_vi.yScale = v & 0xFFF0FFF; break;
    }
}
