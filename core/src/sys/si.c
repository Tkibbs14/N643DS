// Serial Interface and the PIF's joybus: controllers on channels 0-3, EEPROM on channel 4 (n64brew "Joybus").
#include <string.h>
#include "sys.h"
#include "../cpu/vr4300.h"

static u32 g_dramAddr, g_status;
#define SI_DMA_CYCLES 6000  // ~64 us: a PIF round trip (approximate)

void si_reset(void) {
    g_dramAddr = g_status = 0;
    memset(g_mem.pifram, 0, sizeof g_mem.pifram);
}

static u8 pr(int i) { return m_r8(g_mem.pifram, (u32)i); }
static void pw(int i, u8 v) { m_w8(g_mem.pifram, (u32)i, v); }

// One joybus command. tx / rx are byte offsets into PIF RAM. Returns the error bits for the rx-length byte
// (0x80: no device).
static u8 joybus(int ch, int tx, int txLen, int rx, int rxLen) {
    u8 cmd = pr(tx);
    if (ch < 4) {
        const Pad* p = &g_pad[ch];
        if (!p->present) return 0x80;
        switch (cmd) {
        case 0x00: case 0xFF:  // info: standard controller, no pak
            if (rxLen >= 3) { pw(rx, 0x05); pw(rx + 1, 0x00); pw(rx + 2, 0x02); }
            return 0;
        case 0x01:  // buttons and stick
            if (rxLen >= 4) {
                pw(rx, (u8)(p->buttons >> 8));
                pw(rx + 1, (u8)p->buttons);
                pw(rx + 2, (u8)p->x);
                pw(rx + 3, (u8)p->y);
            }
            return 0;
        case 0x02: case 0x03:  // pak read / write: no pak; reads give zeros
            for (int i = 0; i < rxLen; i++) pw(rx + i, 0);
            return 0;
        }
        return 0;
    }
    if (ch == 4 && (g_mem.saveType == SAVE_EEPROM4K || g_mem.saveType == SAVE_EEPROM16K)) {
        switch (cmd) {
        case 0x00: case 0xFF:
            if (rxLen >= 3) {
                pw(rx, 0x00);
                pw(rx + 1, g_mem.saveType == SAVE_EEPROM16K ? 0xC0 : 0x80);
                pw(rx + 2, 0x00);
            }
            return 0;
        case 0x04: {  // read an 8-byte block
            u32 blk = pr(tx + 1), base = (blk * 8) % g_mem.saveSize;
            for (int i = 0; i < 8 && i < rxLen; i++) pw(rx + i, m_r8(g_mem.save, base + (u32)i));
            return 0;
        }
        case 0x05: {  // write an 8-byte block
            u32 blk = pr(tx + 1), base = (blk * 8) % g_mem.saveSize;
            for (int i = 0; i < 8 && 2 + i < txLen; i++) m_w8(g_mem.save, base + (u32)i, pr(tx + 2 + i));
            g_mem.saveDirty = true;
            if (rxLen >= 1) pw(rx, 0x00);
            return 0;
        }
        }
        return 0;
    }
    return 0x80;
}

void pif_process(void) {
    u8 ctl = pr(63);
    if (!(ctl & 1)) return;
    int ch = 0, i = 0;
    while (i < 63 && ch < 6) {
        u8 tx = pr(i);
        if (tx == 0xFE) break;                       // end of commands
        if (tx == 0xFF || tx == 0xFD) { i++; continue; }  // padding
        if (tx == 0) { ch++; i++; continue; }        // this channel skipped
        int txLen = tx & 0x3F;
        if (i + 1 >= 63) break;
        int rxIdx = i + 1;
        int rxLen = pr(rxIdx) & 0x3F;
        int txAt = i + 2, rxAt = txAt + txLen;
        if (rxAt + rxLen > 63) break;
        u8 err = joybus(ch, txAt, txLen, rxAt, rxLen);
        pw(rxIdx, (u8)(rxLen | err));
        i = rxAt + rxLen;
        ch++;
    }
    pw(63, ctl & ~1u);
}

void si_dma_done(void) {
    g_status &= ~3u;
    g_status |= 1u << 12;
    mi_raise(MI_SI);
}

u32 si_read(u32 reg) {
    switch (reg) {
    case 0x00: return g_dramAddr;
    case 0x18: return g_status;
    }
    return 0;
}

void si_write(u32 reg, u32 v, u32 mask) {
    (void)mask;
    switch (reg) {
    case 0x00: g_dramAddr = v & 0xFFFFF8; break;
    case 0x04:  // PIF RAM -> RDRAM
        for (u32 i = 0; i < 64; i += 4) m_w32(g_mem.rdram, (g_dramAddr + i) & (g_mem.rdramSize - 1), m_r32(g_mem.pifram, i));
        code_range_written(g_dramAddr & (g_mem.rdramSize - 1), 64);
        g_status |= 1;
        g_stats.siDmas++;
        sched_at(EV_SI_DMA, g_cpu.cycles + SI_DMA_CYCLES);
        break;
    case 0x10:  // RDRAM -> PIF RAM, then the PIF runs the commands
        for (u32 i = 0; i < 64; i += 4) m_w32(g_mem.pifram, i, m_r32(g_mem.rdram, (g_dramAddr + i) & (g_mem.rdramSize - 1)));
        pif_process();
        g_status |= 1;
        g_stats.siDmas++;
        sched_at(EV_SI_DMA, g_cpu.cycles + SI_DMA_CYCLES);
        break;
    case 0x18: g_status &= ~(1u << 12); mi_clear(MI_SI); break;
    }
}
