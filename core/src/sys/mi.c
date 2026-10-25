// MIPS Interface: the RCP's interrupt controller, routed to the CPU's Cause.IP2.
#include "sys.h"
#include "../cpu/vr4300.h"

static u32 g_mode, g_intr, g_mask;

static void update(void) { vr_set_ip(&g_cpu, 2, (g_intr & g_mask) != 0); }

void mi_raise(u32 bits) { g_intr |= bits; update(); }
void mi_clear(u32 bits) { g_intr &= ~bits; update(); }

u32 mi_read(u32 reg) {
    switch (reg) {
    case 0x0: return g_mode;
    case 0x4: return 0x02020102;  // MI_VERSION: RSP 2, RDP 2, RAC 1, IO 2
    case 0x8: return g_intr;
    case 0xC: return g_mask;
    }
    return 0;
}

void mi_write(u32 reg, u32 v, u32 mask) {
    (void)mask;
    if (reg == 0x0) {
        g_mode = (g_mode & ~0x7Fu) | (v & 0x7F);  // init length
        if (v & (1u << 7)) g_mode &= ~(1u << 7);   // clear / set init mode
        if (v & (1u << 8)) g_mode |= 1u << 7;
        if (v & (1u << 9)) g_mode &= ~(1u << 8);   // clear / set ebus test mode
        if (v & (1u << 10)) g_mode |= 1u << 8;
        if (v & (1u << 11)) mi_clear(MI_DP);       // clear the DP interrupt
        if (v & (1u << 12)) g_mode &= ~(1u << 9);  // clear / set RDRAM register mode
        if (v & (1u << 13)) g_mode |= 1u << 9;
    } else if (reg == 0xC) {
        for (int i = 0; i < 6; i++) {  // pairs of (clear, set) bits per interrupt
            if (v & (1u << (2 * i))) g_mask &= ~(1u << i);
            if (v & (1u << (2 * i + 1))) g_mask |= 1u << i;
        }
        update();
    }
}

void mi_reset(void);
void mi_reset(void) { g_mode = 0; g_intr = 0; g_mask = 0; }
