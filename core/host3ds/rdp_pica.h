#pragma once
// The RDP on the PICA200 (rdp_pica.c).
#include "../src/common.h"

bool rdp_pica_init(void);           // citro3d and the renderer's resources (after gfxInitDefault); false: not available
void rdp_pica_attach(bool on);      // draw through the GPU (on) or the software renderer into RDRAM (off)
void rdp_pica_reset(void);          // forget targets and textures (another cartridge)
// Shows the target the VI scans out (origin: its RDRAM address) on the top screen; false when no target holds it
// (pictures drawn by the CPU: the caller converts RDRAM).
bool rdp_pica_present(u32 origin, u32 width, u32 height);
bool rdp_pica_capture(u8* rgb, u32 width, u32 height);  // the last present as RGB rows (the first call arms it)

typedef struct {
    u64 drawCalls, triangles, rects, depthClears, texDecodes, presents, splits, approxDraws, lleTriangles;
    u64 lookupTicks, drawTicks;  // system ticks in texture lookups (misses decode) and in state + draw calls
    u64 texels, texAllocs, hashBytes;  // texels decoded (power-of-two sizes), textures allocated, TMEM bytes hashed
} RdpPicaStats;
extern RdpPicaStats g_picaStats;
// Debugging: the next g_picaTexDumpN decoded textures (RGBA8888, red on top) go to g_picaTexDump
extern int g_picaTexDumpN;
extern void (*g_picaTexDump)(const u32* rgba, u32 w, u32 h, int tile);
extern int g_picaLogN;  // debugging: the next N textured draws described to g_picaLog
extern void (*g_picaLog)(const char* line);
