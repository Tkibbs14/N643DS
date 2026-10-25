// Physical memory map (n64brew "Memory map"): RDRAM, RCP registers, cartridge domains, PIF.
#include <stdio.h>
#include <string.h>
#include "sys.h"

Mem g_mem;
Pad g_pad[4];
SysStats g_stats;
u8 g_codePage[RDRAM_MAX >> 12];
u64 g_timeRsp, g_timeRdp;
void (*g_onField)(void);
void (*g_onIsv)(const char* s, u32 n);
void (*g_onAudio)(const s16* lr, u32 frames, u32 rate);

void m_copy(u8* dst, u32 da, const u8* src, u32 sa, u32 len) {
    if (!((da | sa | len) & 3)) {
        for (u32 i = 0; i < len; i += 4) m_w32(dst, da + i, m_r32(src, sa + i));
        return;
    }
    for (u32 i = 0; i < len; i++) m_w8(dst, da + i, m_r8(src, sa + i));
}

// ---- Cartridge ----------------------------------------------------------------------------------------------------
static u32 open_bus(u32 pa) { return ((pa & 0xFFFF) << 16) | ((pa + 2) & 0xFFFF); }

static bool isv_window(u32 off) { return off >= 0x03FF0000u && g_mem.romSize <= 0x03FF0000u; }

u32 cart_read32(u32 pa) {
    if (pa >= 0x08000000u && pa < 0x10000000u) {  // domain 2 address 2: SRAM (FlashRAM: not yet)
        if (g_mem.saveType == SAVE_SRAM32K || g_mem.saveType == SAVE_SRAM96K) {
            u32 off = (pa - 0x08000000u) % g_mem.saveSize;
            return m_r32(g_mem.save, off & ~3u);
        }
        return 0;
    }
    if (pa >= 0x10000000u && pa < 0x1FC00000u) {  // domain 1 address 2: ROM
        u32 off = pa - 0x10000000u;
        if (isv_window(off)) return m_r32(g_mem.isv, off & 0xFFFC);
        if (off < g_mem.romSize) return m_r32(g_mem.rom, off & ~3u);
        return open_bus(pa);
    }
    return 0;  // 64DD registers and IPL ROM: no 64DD
}

void cart_write32(u32 pa, u32 v, u32 mask) {
    if (pa >= 0x08000000u && pa < 0x10000000u) {
        if (g_mem.saveType == SAVE_SRAM32K || g_mem.saveType == SAVE_SRAM96K) {
            u32 off = ((pa - 0x08000000u) % g_mem.saveSize) & ~3u;
            m_w32(g_mem.save, off, (m_r32(g_mem.save, off) & ~mask) | (v & mask));
            g_mem.saveDirty = true;
        }
        return;
    }
    if (pa >= 0x10000000u && pa < 0x1FC00000u) {
        u32 off = pa - 0x10000000u;
        if (!isv_window(off)) return;  // ROM writes go nowhere
        off &= 0xFFFC;
        m_w32(g_mem.isv, off, (m_r32(g_mem.isv, off) & ~mask) | (v & mask));
        if (off == 0x14 && v) {  // the length register: print the bytes at 0x20
            static char buf[0x10000];  // static: the 3DS host's stack is small (a 64 KB local overflowed it)
            u32 n = v < 0xFFE0 ? v : 0xFFE0;
            for (u32 i = 0; i < n; i++) buf[i] = (char)m_r8(g_mem.isv, 0x20 + i);
            if (g_onIsv) g_onIsv(buf, n);
            m_w32(g_mem.isv, 0x14, 0);
        }
    }
}

// ---- Dispatch -----------------------------------------------------------------------------------------------------
u32 bus_r32(u32 pa) {
    if (pa < g_mem.rdramSize) return m_r32(g_mem.rdram, pa);
    if (pa < 0x03F00000u) return 0;
    if (pa < 0x04000000u) return rdreg_read(pa & 0xFFFFF);
    if (pa < 0x04040000u) return m_r32(g_mem.spmem, pa & 0x1FFC);
    if (pa < 0x04080000u) return sp_read(pa & 0x1F);
    if (pa < 0x04100000u) return sp_pc_read(pa & 0xF);
    if (pa < 0x04200000u) return dp_read(pa & 0x1F);
    if (pa < 0x04300000u) return 0;  // DP span registers
    switch (pa >> 20) {
    case 0x043: return mi_read(pa & 0xF);
    case 0x044: return vi_read(pa & 0x3F);
    case 0x045: return ai_read(pa & 0x1F);
    case 0x046: return pi_read(pa & 0x3F);
    case 0x047: return ri_read(pa & 0x1F);
    case 0x048: return si_read(pa & 0x1F);
    }
    if (pa >= 0x05000000u && pa < 0x1FC00000u) return cart_read32(pa);
    if (pa >= 0x1FC007C0u && pa < 0x1FC00800u) return m_r32(g_mem.pifram, pa & 0x3C);
    return 0;  // PIF boot ROM (not present: the boot is high-level, rom.c) and unmapped space
}

void bus_w32(u32 pa, u32 v, u32 mask) {
    if (pa < g_mem.rdramSize) {
        if (g_codePage[pa >> 12]) jit_code_written(pa, 4);
        m_w32(g_mem.rdram, pa, (m_r32(g_mem.rdram, pa) & ~mask) | (v & mask));
        return;
    }
    if (pa < 0x03F00000u) return;
    if (pa < 0x04000000u) { rdreg_write(pa & 0xFFFFF, v, mask); return; }
    if (pa < 0x04040000u) {
        u32 a = pa & 0x1FFC;
        m_w32(g_mem.spmem, a, (m_r32(g_mem.spmem, a) & ~mask) | (v & mask));
        return;
    }
    if (pa < 0x04080000u) { sp_write(pa & 0x1F, v, mask); return; }
    if (pa < 0x04100000u) { sp_pc_write(pa & 0xF, v, mask); return; }
    if (pa < 0x04200000u) { dp_write(pa & 0x1F, v, mask); return; }
    if (pa < 0x04300000u) return;
    switch (pa >> 20) {
    case 0x043: mi_write(pa & 0xF, v, mask); return;
    case 0x044: vi_write(pa & 0x3F, v, mask); return;
    case 0x045: ai_write(pa & 0x1F, v, mask); return;
    case 0x046: pi_write(pa & 0x3F, v, mask); return;
    case 0x047: ri_write(pa & 0x1F, v, mask); return;
    case 0x048: si_write(pa & 0x1F, v, mask); return;
    }
    if (pa >= 0x05000000u && pa < 0x1FC00000u) { cart_write32(pa, v, mask); return; }
    if (pa >= 0x1FC007C0u && pa < 0x1FC00800u) {
        u32 a = pa & 0x3C;
        m_w32(g_mem.pifram, a, (m_r32(g_mem.pifram, a) & ~mask) | (v & mask));
    }
}

u64 bus_r64(u32 pa) { return ((u64)bus_r32(pa) << 32) | bus_r32(pa + 4); }
void bus_w64(u32 pa, u64 v) {
    bus_w32(pa, (u32)(v >> 32), 0xFFFFFFFFu);
    bus_w32(pa + 4, (u32)v, 0xFFFFFFFFu);
}

// Narrow accesses use the word's byte lanes (big-endian: byte 0 is bits 31:24). A narrow store to an RCP register
// writes the whole register with the data shifted into its lane, as the bus does.
u16 bus_r16(u32 pa) { return (u16)(bus_r32(pa & ~3u) >> ((2 - (pa & 2)) * 8)); }
u8 bus_r8(u32 pa) { return (u8)(bus_r32(pa & ~3u) >> ((3 - (pa & 3)) * 8)); }
void bus_w16(u32 pa, u16 v) {
    int sh = (2 - (pa & 2)) * 8;
    bus_w32(pa & ~3u, (u32)v << sh, 0xFFFFu << sh);
}
void bus_w8(u32 pa, u8 v) {
    int sh = (3 - (pa & 3)) * 8;
    bus_w32(pa & ~3u, (u32)v << sh, 0xFFu << sh);
}
