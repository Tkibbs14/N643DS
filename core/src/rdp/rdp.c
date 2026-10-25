// RDP command stream: commands come from RDRAM (or DMEM in XBUS mode) between DP_CURRENT and DP_END. This file splits
// the stream into commands (a triangle is 32 to 176 bytes, a texture rectangle 16, everything else 8), keeps a partial
// command for the next kick, and hands each command to the renderer (rdp_command, software renderer next).
#include <string.h>
#include "../sys/sys.h"

RdpStats g_rdpStats;
void rdp_full_sync(void);
void rdp_command(const u64* words, int n);  // rdp_soft.c (or the 3DS renderer)

static u64 g_buf[24];  // the longest command: 176 bytes
static int g_have, g_need;

static int cmd_words(u32 id) {
    switch (id) {
    case 0x08: return 4;   // fill triangle
    case 0x09: return 6;   // + z
    case 0x0A: return 12;  // + texture
    case 0x0B: return 14;  // + texture, z
    case 0x0C: return 12;  // shade
    case 0x0D: return 14;  // shade, z
    case 0x0E: return 20;  // shade, texture
    case 0x0F: return 22;  // shade, texture, z
    case 0x24: case 0x25: return 2;  // texture rectangle (flip)
    default: return 1;
    }
}

void rdp_reset(void) {
    g_have = g_need = 0;
    memset(&g_rdpStats, 0, sizeof g_rdpStats);
}

static u64 read_word(u32 a, bool xbus) {
    if (xbus) {
        a &= 0xFF8;
        return ((u64)m_r32(g_mem.spmem, a) << 32) | m_r32(g_mem.spmem, a + 4);
    }
    a &= (g_mem.rdramSize - 1) & ~7u;
    return m_r64(g_mem.rdram, a);
}

void rdp_process(u32 start, u32 end, bool xbus) {
    u64 t0 = host_ticks();
    for (u32 a = start; a < end; a += 8) {
        u64 w = read_word(a, xbus);
        g_rdpStats.bytes += 8;
        if (g_have == 0) g_need = cmd_words((u32)(w >> 56) & 0x3F);
        g_buf[g_have++] = w;
        if (g_have < g_need) continue;
        u32 id = (u32)(g_buf[0] >> 56) & 0x3F;
        g_rdpStats.commands++;
        if (id >= 0x08 && id <= 0x0F) g_rdpStats.triangles++;
        else if (id == 0x24 || id == 0x25 || id == 0x36) g_rdpStats.rects++;
        rdp_command(g_buf, g_need);
        if (id == 0x29) {  // SYNC_FULL: the DP interrupt when done
            g_rdpStats.syncFull++;
            rdp_full_sync();
        }
        g_have = 0;
    }
    g_timeRdp += host_ticks() - t0;
}
