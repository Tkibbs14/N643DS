// Signal Processor (RSP) interface and the RDP's command registers. The RSP itself is not emulated yet: a started
// task is read from the OSTask block at DMEM 0xFC0 and completed after a fixed time, as if the microcode had run
// (docs/DESIGN.md "RSP": high-level graphics and audio come next, checked against a low-level RSP on the PC).
#include <string.h>
#include "sys.h"
#include "../cpu/vr4300.h"

// SP_STATUS bits (read view)
#define SPS_HALT (1u << 0)
#define SPS_BROKE (1u << 1)
#define SPS_DMABUSY (1u << 2)
#define SPS_DMAFULL (1u << 3)
#define SPS_IOFULL (1u << 4)
#define SPS_SSTEP (1u << 5)
#define SPS_IBREAK (1u << 6)
#define SPS_SIG(n) (1u << (7 + (n)))

static u32 g_memAddr, g_dramAddr, g_rdLen, g_wrLen, g_status, g_semaphore, g_pc;
static u32 g_taskType;
// Fixed task lengths until the high-level microcode exists (docs/DESIGN.md "Timing").
#define TASK_CYCLES_GFX 150000
#define TASK_CYCLES_AUDIO 40000
#define TASK_CYCLES_OTHER 40000

void sp_reset(void) {
    g_memAddr = g_dramAddr = g_rdLen = g_wrLen = g_semaphore = g_pc = 0;
    g_status = SPS_HALT;
}

static void dma(bool toSp, u32 lenReg) {
    u32 len = (lenReg & 0xFFF) | 7;
    u32 count = ((lenReg >> 12) & 0xFF) + 1;
    u32 skip = (lenReg >> 20) & 0xFF8;
    u32 mem = g_memAddr & 0x1FF8, bank = mem & 0x1000;
    u32 dram = g_dramAddr & 0xFFFFF8;
    for (u32 r = 0; r < count; r++) {
        for (u32 i = 0; i <= len; i += 4) {
            u32 m = bank | ((mem + i) & 0xFFF);
            u32 d = (dram + i) & (g_mem.rdramSize - 1);
            if (toSp) m_w32(g_mem.spmem, m, m_r32(g_mem.rdram, d));
            else { m_w32(g_mem.rdram, d, m_r32(g_mem.spmem, m)); if (g_codePage[d >> 12]) jit_code_written(d, 4); }
        }
        mem = bank | ((mem + len + 1) & 0xFFF);
        dram += len + 1 + skip;
    }
    g_memAddr = mem;
    g_dramAddr = dram & 0xFFFFF8;
    if (toSp) g_rdLen = 0xFF8 | (skip << 20);
    else g_wrLen = 0xFF8 | (skip << 20);
}

static void task_start(void) {
    g_taskType = m_r32(g_mem.spmem, 0xFC0);
    g_stats.spTasks[g_taskType & 7]++;
    u32 cycles = g_taskType == 1 ? TASK_CYCLES_GFX : g_taskType == 2 ? TASK_CYCLES_AUDIO : TASK_CYCLES_OTHER;
    sched_at(EV_SP_TASK, g_cpu.cycles + cycles);
}

void sp_task_done(void) {
    if (g_status & SPS_HALT) return;  // halted by the CPU meanwhile
    g_status |= SPS_HALT | SPS_BROKE | SPS_SIG(2);  // libultra's microcodes set SIG2 ("task done") and break
    if (g_status & SPS_IBREAK) mi_raise(MI_SP);
    if (g_taskType == 1) {
        // The graphics microcode ends with an RDP full sync; with no RDP yet, its interrupt comes with the task.
        g_stats.dpKicks++;
        mi_raise(MI_DP);
    }
}

u32 sp_read(u32 reg) {
    switch (reg) {
    case 0x00: return g_memAddr;
    case 0x04: return g_dramAddr;
    case 0x08: return g_rdLen;
    case 0x0C: return g_wrLen;
    case 0x10: return g_status;
    case 0x14: return 0;  // DMA full
    case 0x18: return 0;  // DMA busy
    case 0x1C: { u32 v = g_semaphore; g_semaphore = 1; return v; }
    }
    return 0;
}

void sp_write(u32 reg, u32 v, u32 mask) {
    (void)mask;
    switch (reg) {
    case 0x00: g_memAddr = v & 0x1FF8; break;
    case 0x04: g_dramAddr = v & 0xFFFFF8; break;
    case 0x08: g_rdLen = v; dma(true, v); break;
    case 0x0C: g_wrLen = v; dma(false, v); break;
    case 0x10: {
        bool wasHalted = g_status & SPS_HALT;
        if (v & (1u << 0)) g_status &= ~SPS_HALT;
        if (v & (1u << 1)) g_status |= SPS_HALT;
        if (v & (1u << 2)) g_status &= ~SPS_BROKE;
        if (v & (1u << 3)) mi_clear(MI_SP);
        if (v & (1u << 4)) mi_raise(MI_SP);
        if (v & (1u << 5)) g_status &= ~SPS_SSTEP;
        if (v & (1u << 6)) g_status |= SPS_SSTEP;
        if (v & (1u << 7)) g_status &= ~SPS_IBREAK;
        if (v & (1u << 8)) g_status |= SPS_IBREAK;
        for (int s = 0; s < 8; s++) {
            if (v & (1u << (9 + 2 * s))) g_status &= ~SPS_SIG(s);
            if (v & (1u << (10 + 2 * s))) g_status |= SPS_SIG(s);
        }
        if (wasHalted && !(g_status & SPS_HALT)) task_start();
        if (!wasHalted && (g_status & SPS_HALT)) sched_cancel(EV_SP_TASK);
        break;
    }
    case 0x1C: g_semaphore = 0; break;
    }
}

u32 sp_pc_read(u32 reg) { return reg == 0 ? g_pc : 0; }
void sp_pc_write(u32 reg, u32 v, u32 mask) {
    (void)mask;
    if (reg == 0) g_pc = v & 0xFFC;
}

// ---- RDP command registers (DP_*) ---------------------------------------------------------------------------------
static u32 g_dpStart, g_dpEnd, g_dpCurrent, g_dpStatus;

void dp_reset(void) {
    g_dpStart = g_dpEnd = g_dpCurrent = 0;
    g_dpStatus = 0x80;  // command buffer ready
}

void dp_done_event(void) {}

u32 dp_read(u32 reg) {
    switch (reg) {
    case 0x00: return g_dpStart;
    case 0x04: return g_dpEnd;
    case 0x08: return g_dpCurrent;
    case 0x0C: return g_dpStatus;
    }
    return 0;
}

void dp_write(u32 reg, u32 v, u32 mask) {
    (void)mask;
    switch (reg) {
    case 0x00: g_dpStart = v & 0xFFFFF8; g_dpCurrent = g_dpStart; break;
    case 0x04:
        g_dpEnd = v & 0xFFFFF8;
        g_dpCurrent = g_dpEnd;  // no RDP yet: the commands are consumed at once
        g_stats.dpKicks++;
        break;
    case 0x0C:
        if (v & (1u << 0)) g_dpStatus &= ~1u;  // XBUS DMEM DMA
        if (v & (1u << 1)) g_dpStatus |= 1u;
        if (v & (1u << 2)) g_dpStatus &= ~2u;  // freeze
        if (v & (1u << 3)) g_dpStatus |= 2u;
        if (v & (1u << 4)) g_dpStatus &= ~4u;  // flush
        if (v & (1u << 5)) g_dpStatus |= 4u;
        break;
    }
}
