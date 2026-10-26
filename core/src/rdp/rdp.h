#pragma once
// The RDP's state as the software renderer keeps it (rdp_soft.c), for renderers that draw elsewhere (the 3DS GPU,
// core/host3ds/rdp_pica.c): they let rdp_soft.c follow every state and texture-load command and draw what it hands
// them through g_rdpGpu.
#include "../common.h"

typedef struct {
    u32 fmt, size, line, tmem, palette;
    u32 cms, cmt, masks, maskt, shifts, shiftt;  // cm: bit 0 mirror, bit 1 clamp
    u32 sl, tl, sh, th;                          // 10.2
} RdpTile;

typedef struct {
    u32 ciAddr, ciFmt, ciSize, ciWidth;          // colour image
    u32 zAddr;
    u32 tiAddr, tiFmt, tiSize, tiWidth;          // texture image
    u32 sxh, syh, sxl, syl;                      // scissor, 10.2
    u64 om;                                      // other modes (the whole command word)
    u64 combine;
    u32 fill, prim, env, blend, fog;             // RGBA8888 colours (fill: raw)
    u32 primLod, primMinLod, primZ;
    RdpTile tiles[8];
    u8 tmem[4096];
} RdpState;

extern RdpState g_rdp;
#define RDP_OM(bit, n) ((u32)(g_rdp.om >> (bit)) & ((1u << (n)) - 1))
enum { RDP_CYC1 = 0, RDP_CYC2 = 1, RDP_COPY = 2, RDP_FILL = 3 };

// The texel at integer (s, t) of a tile (already wrapped / clamped into it), RGBA8888 with red in the top byte.
u32 rdp_texel(int tile, int s, int t);

// Drawing handed to another renderer instead of the software one (NULL: rdp_soft.c draws). Coordinates as the
// commands give them; the state is g_rdp at the time of the call.
typedef struct {
    void (*fill_rect)(u64 w);                     // FILL_RECTANGLE
    void (*tex_rect)(const u64* w, bool flip);    // TEXTURE_RECTANGLE (two words)
    void (*triangle)(const u64* w, int n);        // an RDP triangle command (the RSP interpreter's path)
} RdpGpu;
extern const RdpGpu* g_rdpGpu;
// Changes whenever a texture could decode differently (loads, tiles, TLUT / filter modes): a GPU renderer's texture
// cache looks up again only then.
extern u32 g_rdpTexGen;

// A whole tile's texels in rows of tw (RGBA8888, red on top), as rdp_texel at every (s, t) inside tw x th
void rdp_decode_tile(int tile, u32 tw, u32 th, u32* out);
