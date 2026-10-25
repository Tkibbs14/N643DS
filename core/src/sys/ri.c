// RDRAM Interface and the RDRAM chips' registers: stored and read back. The high-level boot does not configure them;
// running a cartridge's own IPL3 will need their real behaviour (docs/DESIGN.md "Boot").
#include "sys.h"

static u32 g_ri[8];
static u32 g_rdreg[16];

u32 ri_read(u32 reg) { return g_ri[(reg >> 2) & 7]; }
void ri_write(u32 reg, u32 v, u32 mask) { (void)mask; g_ri[(reg >> 2) & 7] = v; }
u32 rdreg_read(u32 reg) { return g_rdreg[(reg >> 2) & 15]; }
void rdreg_write(u32 reg, u32 v, u32 mask) { (void)mask; g_rdreg[(reg >> 2) & 15] = v; }
