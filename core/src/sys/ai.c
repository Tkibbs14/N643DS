// Audio Interface: a two-entry DMA FIFO playing RDRAM samples (big-endian s16 left / right) at VI clock / (DACRATE+1).
// The interrupt fires when a buffer starts playing, which is when the FIFO has room again (n64brew "Audio Interface").
#include "sys.h"
#include "../cpu/vr4300.h"

typedef struct { u32 addr, len; } AiBuf;
static u32 g_dramAddr, g_control, g_dacrate, g_bitrate;
static AiBuf g_fifo[2];
static int g_count;

static u32 sample_rate(void) {
    u32 clock = g_mem.pal ? VI_CLOCK_PAL : VI_CLOCK_NTSC;
    return clock / (g_dacrate + 1);
}

static void start_current(void) {
    AiBuf* b = &g_fifo[0];
    u32 frames = b->len / 4;
    u32 rate = sample_rate();
    if (g_onAudio && frames && rate) {
        static s16 out[2 * 0x10000];
        for (u32 i = 0; i < frames; i++) {
            u32 w = m_r32(g_mem.rdram, (b->addr + i * 4) & (g_mem.rdramSize - 1) & ~3u);
            out[2 * i] = (s16)(w >> 16);
            out[2 * i + 1] = (s16)w;
        }
        g_onAudio(out, frames, rate);
    }
    u64 dur = rate ? (u64)frames * CPU_HZ / rate : 0;
    if (dur < 64) dur = 64;
    sched_at(EV_AI_BUF, g_cpu.cycles + dur);
    g_stats.aiBufs++;
    mi_raise(MI_AI);
}

void ai_reset(void) {
    g_dramAddr = g_control = g_dacrate = g_bitrate = 0;
    g_count = 0;
}

void ai_buf_event(void) {
    if (g_count == 0) return;
    g_fifo[0] = g_fifo[1];
    g_count--;
    if (g_count) start_current();
}

u32 ai_read(u32 reg) {
    switch (reg) {
    case 0x04: return g_count ? g_fifo[0].len : 0;  // AI_LEN reads the remaining length; whole-buffer granularity here
    case 0x0C: {
        u32 full = g_count >= 2, busy = g_count >= 1;
        return (full << 31) | (busy << 30) | (1u << 24) | (1u << 20) | full;
    }
    }
    return 0;
}

void ai_write(u32 reg, u32 v, u32 mask) {
    (void)mask;
    switch (reg) {
    case 0x00: g_dramAddr = v & 0xFFFFF8; break;
    case 0x04: {
        u32 len = v & 0x3FFF8;
        if (!len || g_count >= 2) break;
        g_fifo[g_count].addr = g_dramAddr;
        g_fifo[g_count].len = len;
        g_count++;
        if (g_count == 1) start_current();
        break;
    }
    case 0x08: g_control = v & 1; break;
    case 0x0C: mi_clear(MI_AI); break;
    case 0x10: g_dacrate = v & 0x3FFF; break;
    case 0x14: g_bitrate = v & 0xF; break;
    }
}
