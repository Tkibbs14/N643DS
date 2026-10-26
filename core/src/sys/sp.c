// Signal Processor (RSP) interface and the RDP's command registers. The RSP runs on the interpreter in src/rsp
// (g_rspMode RSP_LLE, the default) or is stubbed (RSP_STUB: tasks complete after a fixed time, nothing is drawn).
#include <stdio.h>
#include <string.h>
#include "sys.h"
#include "../cpu/vr4300.h"
#include "../rsp/rsp.h"
#include "../hle/hle.h"

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

static bool g_hleInFlight;     // a high-level graphics task submitted and not yet ended (emulated)
static bool g_hleOutstanding;  // a high-level graphics task whose results were not taken yet
static u64 g_hleNextCycles;    // RSP cycles the next high-level task takes: the previous task's estimate
static bool g_hleNextSync, g_hleEndSync;  // the previous task ended with an RDP full sync; the running task's copy
static void hle_take_results(void);

void sp_reset(void) {
    g_hleInFlight = g_hleOutstanding = false;
    g_hleNextCycles = 200000;
    g_hleNextSync = g_hleEndSync = true;
    g_memAddr = g_dramAddr = g_rdLen = g_wrLen = g_semaphore = g_pc = 0;
    g_status = SPS_HALT;
}

static void dma(bool toSp, u32 lenReg) {
    u32 len = (lenReg & 0xFFF) | 7;
    u32 count = ((lenReg >> 12) & 0xFF) + 1;
    u32 skip = (lenReg >> 20) & 0xFF8;
    u32 mem = g_memAddr & 0x1FF8, bank = mem & 0x1000;
    u32 dram = g_dramAddr & 0xFFFFF8;
    if (g_traceExc)
        fprintf(stderr, "  SP DMA %s: %s 0x%03x, RDRAM 0x%06x, %u x %u bytes, skip %u\n", toSp ? "to SP" : "from SP",
                bank ? "IMEM" : "DMEM", (unsigned)(mem & 0xFFF), (unsigned)dram, (unsigned)count, (unsigned)(len + 1),
                (unsigned)skip);
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

// The RSP: g_rspMode RSP_LLE runs the microcode in IMEM on the interpreter (src/rsp) as soon as the CPU clears HALT,
// to its BREAK; what the CPU would notice when the RSP finishes (HALT, BROKE, the SP interrupt, an RDP full sync's
// interrupt) happens after the RSP's own run time (1.5 CPU cycles per RSP cycle: 93.75 / 62.5 MHz). RSP_STUB only
// completes tasks after a fixed time (no graphics, no audio).
int g_rspMode = RSP_LLE;
#define RSP_MAX_CYCLES 50000000ull  // a microcode that never reaches its BREAK
static bool g_rspRunning, g_rspBroke, g_dpIntrPending, g_hleDone;
u64 g_rspTotalCycles;

static void task_start(void) {
    g_taskType = m_r32(g_mem.spmem, 0xFC0);
    g_stats.spTasks[g_taskType & 7]++;
    if (g_traceExc && g_taskType == 1 && g_stats.spTasks[1] == 1) {
        // The graphics microcode's name: the "RSP ... " string in its data segment (OSTask ucode_data at DMEM 0xFD8)
        u32 data = m_r32(g_mem.spmem, 0xFD8) & 0xFFFFFF, size = m_r32(g_mem.spmem, 0xFDC);
        char s[96];
        u32 n = 0;
        for (u32 i = 0; i + 3 < size && i < 0x1000 && n == 0; i++)
            if (m_r8(g_mem.rdram, data + i) == 'R' && m_r8(g_mem.rdram, data + i + 1) == 'S' && m_r8(g_mem.rdram, data + i + 2) == 'P')
                for (; n < sizeof s - 1; n++) {
                    u8 ch = m_r8(g_mem.rdram, data + i + n);
                    if (ch < 32 || ch > 126) break;
                    s[n] = (char)ch;
                }
        s[n] = 0;
        fprintf(stderr, "  graphics microcode: text 0x%06x, data 0x%06x (%u bytes): \"%s\"\n",
                (unsigned)(m_r32(g_mem.spmem, 0xFD0) & 0xFFFFFF), (unsigned)data, (unsigned)size, n ? s : "(no RSP string)");
    }
    if (g_hleGfx && g_taskType == 1) {
        hle_take_results();  // the previous task finished (core 0 waits for a worker still running it)
        if (hle_gfx_prepare()) {
            // High-level graphics microcode: the display list runs natively, on core 2 when the host has a worker
            // (g_hleAsync), else now. Its end (the microcode's HALT | BROKE | SIG2, and the RDP's full-sync interrupt
            // when the previous task had one) comes after the previous task's estimated run time: the same on every
            // host, as a worker may still be running this one when it ends (g_hleAhead).
            u64 ran = g_hleNextCycles;
            g_hleEndSync = g_hleNextSync;
            g_rspTotalCycles += ran;
            if (g_hleAsync) {
                g_hleAsync->submit();
            } else {
                u64 t0 = host_ticks();
                u8 phase = g_phase;
                g_phase = PH_HLE;
                hle_gfx_run();
                g_phase = phase;
                g_timeRsp += host_ticks() - t0;
            }
            g_hleOutstanding = g_hleInFlight = true;
            g_rspBroke = g_hleDone = true;
            sched_at(EV_SP_TASK, g_cpu.cycles + ran * 3 / 2 + 1);
            return;
        }
    }
    if (g_rspMode == RSP_STUB) {
        u32 cycles = g_taskType == 1 ? TASK_CYCLES_GFX : g_taskType == 2 ? TASK_CYCLES_AUDIO : TASK_CYCLES_OTHER;
        sched_at(EV_SP_TASK, g_cpu.cycles + cycles);
        return;
    }
    bool compare = false;
    if (g_hleCompare && g_taskType == 1) {  // HLE first from the same start, then the interpreter (src/hle/compare.c)
        g_rspRunning = true;
        compare = hle_compare_before();
        g_rspRunning = g_dpIntrPending = false;
    }
    g_rsp.pc = g_pc & 0xFFC;
    g_rsp.inDelay = g_rsp.branchPending = false;
    g_rspRunning = true;
    u64 t0 = host_ticks();
    u64 ran = rsp_run(RSP_MAX_CYCLES);
    g_timeRsp += host_ticks() - t0;
    g_rspRunning = false;
    if (compare) hle_compare_after();
    g_rspTotalCycles += ran;
    g_pc = g_rsp.pc;
    g_rspBroke = g_rsp.halted;
    if (g_traceExc)
        fprintf(stderr, "  RSP task type %u: %llu RSP cycles, %s, pc %03x\n", (unsigned)g_taskType, (unsigned long long)ran,
                g_rsp.halted ? "break" : "NO BREAK (cycle limit)", (unsigned)g_rsp.pc);
    if (g_traceExc && !g_rsp.halted) {  // the loop it is stuck in
        for (u32 a = (g_rsp.pc - 0x20) & 0xFFC, k = 0; k < 16; k++, a = (a + 4) & 0xFFC)
            fprintf(stderr, "    imem %03x: %08x\n", (unsigned)a, (unsigned)m_r32(g_mem.spmem, 0x1000 | a));
        for (int r = 0; r < 32; r++) fprintf(stderr, "%s r%d=%08x", r % 8 ? "" : "\n   ", r, (unsigned)g_rsp.r[r]);
        fprintf(stderr, "\n");
    }
    sched_at(EV_SP_TASK, g_cpu.cycles + ran * 3 / 2 + 1);
}

// The finished high-level task's results: the next task's run time and whether its end raises the DP interrupt.
// Taken when the next task starts (identically on every host); core 0 waits here for a worker still running it.
static void hle_take_results(void) {
    if (!g_hleOutstanding) return;
    if (g_hleAsync) {
        u8 phase = g_phase;
        g_phase = PH_HLE;
        g_hleAsync->wait();
        g_phase = phase;
    }
    g_hleOutstanding = false;
    g_hleNextSync = hle_last_full_sync();
    g_hleNextCycles = 2000 + hle_last_commands() * 40 + hle_last_triangles() * 150;
}

// A high-level task's emulated end. Unless the host lets the CPU run ahead of the worker (g_hleAhead: the CPU may
// then rewrite what the worker still reads, which is safe for games that double-buffer their display lists and with
// a renderer that writes nothing to RDRAM), core 0 waits here for the worker.
static void hle_task_end(void) {
    if (!g_hleInFlight) return;
    g_hleInFlight = false;
    if (g_hleAsync && !g_hleAhead) {
        u8 phase = g_phase;
        g_phase = PH_HLE;
        g_hleAsync->wait();
        g_phase = phase;
    }
    if (g_hleEndSync) g_dpIntrPending = true;
}

void sp_task_done(void) {
    hle_task_end();
    if (g_rspMode == RSP_LLE || g_hleDone) {  // the interpreter ran it, or the high-level microcode did
        if (g_rspBroke) {
            g_status |= SPS_HALT | SPS_BROKE;
            if (g_hleDone) g_status |= SPS_SIG(2);  // what libultra's microcodes write before their BREAK
            g_hleDone = false;
            if (g_status & SPS_IBREAK) mi_raise(MI_SP);
            g_rspBroke = false;
        }
        if (g_dpIntrPending) { g_dpIntrPending = false; mi_raise(MI_DP); }
        return;
    }
    if (g_status & SPS_HALT) return;  // halted by the CPU meanwhile
    g_status |= SPS_HALT | SPS_BROKE | SPS_SIG(2);  // libultra's microcodes set SIG2 ("task done") and break
    if (g_status & SPS_IBREAK) mi_raise(MI_SP);
    if (g_taskType == 1) {
        // The graphics microcode ends with an RDP full sync; with no RDP, its interrupt comes with the task.
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
        if (g_rspRunning) {  // the microcode wrote its own status: setting HALT stops it
            if (g_status & SPS_HALT) g_rsp.halted = true;
            break;
        }
        if (wasHalted && !(g_status & SPS_HALT)) task_start();
        else if (!wasHalted && (g_status & SPS_HALT) && g_rspMode == RSP_STUB) { sched_cancel(EV_SP_TASK); hle_task_end(); g_hleDone = g_rspBroke = false; }
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

void dp_done_event(void) { mi_raise(MI_DP); }

// An RDP full sync: the DP interrupt, when the RDP would have finished. Commands from a running RSP task wait for the
// task's end (the RSP and RDP run alongside); commands the CPU sent directly get a short delay.
void rdp_full_sync(void);
void rdp_full_sync(void) {
    if (g_rspRunning) g_dpIntrPending = true;
    else sched_at(EV_DP_DONE, g_cpu.cycles + 1000);
}

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
        g_stats.dpKicks++;
        if (g_rspMode == RSP_LLE) rdp_process(g_dpCurrent, g_dpEnd, (g_dpStatus & 1) != 0);
        g_dpCurrent = g_dpEnd;  // the commands are consumed at once
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
