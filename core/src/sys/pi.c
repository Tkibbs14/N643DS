// Peripheral Interface: cartridge DMA. Data moves at the write; the busy time and interrupt follow a flat cost per
// byte (an approximation of the domain timing registers, docs/DESIGN.md "Timing").
#include "sys.h"
#include <stdio.h>
#include "../cpu/vr4300.h"

static u32 g_dramAddr, g_cartAddr, g_status;
static u32 g_dom[8];  // BSD_DOM1_LAT, PWD, PGS, RLS, BSD_DOM2_LAT, PWD, PGS, RLS
#define PI_CYCLES_PER_BYTE 9

void pi_reset(void) {
    g_dramAddr = g_cartAddr = g_status = 0;
    for (int i = 0; i < 8; i++) g_dom[i] = 0;
}

void pi_set_dom1(u32 lat, u32 pwd, u32 pgs, u32 rls);
void pi_set_dom1(u32 lat, u32 pwd, u32 pgs, u32 rls) {
    g_dom[0] = lat;
    g_dom[1] = pwd;
    g_dom[2] = pgs;
    g_dom[3] = rls;
}

static u8 cart_byte(u32 a) {
    u32 off = a - 0x10000000u;
    if (a >= 0x10000000u && off < g_mem.romSize) return m_r8(g_mem.rom, off);
    return (u8)(cart_read32(a & ~3u) >> ((3 - (a & 3)) * 8));
}

static void start(u32 len) {
    g_status |= 1;
    g_stats.piDmas++;
    sched_at(EV_PI_DMA, g_cpu.cycles + 100 + (u64)len * PI_CYCLES_PER_BYTE);
}

void pi_dma_done(void) {
    g_status &= ~3u;
    g_status |= 8;
    mi_raise(MI_PI);
}

u32 pi_read(u32 reg) {
    switch (reg) {
    case 0x00: return g_dramAddr;
    case 0x04: return g_cartAddr;
    case 0x08: case 0x0C: return 0x7F;
    case 0x10: return g_status;
    default: if (reg >= 0x14 && reg <= 0x30) return g_dom[(reg - 0x14) / 4];
    }
    return 0;
}

void pi_write(u32 reg, u32 v, u32 mask) {
    (void)mask;
    switch (reg) {
    case 0x00: g_dramAddr = v & 0xFFFFFE; break;
    case 0x04: g_cartAddr = v & ~1u; break;
    case 0x08: {  // RD_LEN: RDRAM to cartridge (SRAM, the ISViewer window)
        u32 len = (v & 0xFFFFFF) + 1;
        for (u32 i = 0; i < len; i++) {
            u32 a = g_cartAddr + i;
            u8 b = m_r8(g_mem.rdram, (g_dramAddr + i) & (g_mem.rdramSize - 1));
            int sh = (3 - (a & 3)) * 8;
            cart_write32(a & ~3u, (u32)b << sh, 0xFFu << sh);
        }
        g_dramAddr = (g_dramAddr + len + 7) & 0xFFFFF8;
        g_cartAddr = (g_cartAddr + len + 1) & ~1u;
        start(len);
        break;
    }
    case 0x0C: {  // WR_LEN: cartridge to RDRAM
        u32 len = (v & 0xFFFFFF) + 1;
        u32 off = g_cartAddr - 0x10000000u;
        if (g_traceExc) fprintf(stderr, "  PI DMA cart 0x%08x -> RDRAM 0x%06x, %u bytes\n", (unsigned)g_cartAddr, (unsigned)g_dramAddr, (unsigned)len);
        if (g_cartAddr >= 0x10000000u && off + len <= g_mem.romSize && g_dramAddr + len <= g_mem.rdramSize) {
            m_copy(g_mem.rdram, g_dramAddr, g_mem.rom, off, len);
        } else {
            for (u32 i = 0; i < len; i++) {
                u32 d = g_dramAddr + i;
                if (d < g_mem.rdramSize) m_w8(g_mem.rdram, d, cart_byte(g_cartAddr + i));
            }
        }
        code_range_written(g_dramAddr, len);
        g_dramAddr = (g_dramAddr + len + 7) & 0xFFFFF8;
        g_cartAddr = (g_cartAddr + len + 1) & ~1u;
        start(len);
        break;
    }
    case 0x10:
        if (v & 1) { g_status = 0; sched_cancel(EV_PI_DMA); }
        if (v & 2) { g_status &= ~8u; mi_clear(MI_PI); }
        break;
    default:
        if (reg >= 0x14 && reg <= 0x30) g_dom[(reg - 0x14) / 4] = v & 0xFF;
        break;
    }
}
