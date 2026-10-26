// The RDP on the PICA200 (citro3d). The high-level microcode (src/hle/gfx.c) hands over clipped polygons in screen
// space (g_hleVtxSink); the software RDP (src/rdp/rdp_soft.c) still follows every state and texture-load command and
// hands over fill and texture rectangles (g_rdpGpu). Each N64 colour image is a 512 x 256 render target in VRAM; the
// combiner becomes TEV stages (src/rdp/cc_pica.c), textures are decoded from TMEM and cached by a hash of what they
// were decoded from, and the blender's common forms become PICA blending. The picture the VI shows is drawn to the
// top screen from its target (rdp_pica_present). Nothing is written back to RDRAM yet (framebuffer effects read the
// old contents), and triangles from microcodes the HLE does not know are not drawn.
#include <3ds.h>
#include <citro3d.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "rdp_pica.h"
#include "../src/sys/sys.h"
#include "../src/rdp/rdp.h"
#include "../src/rdp/cc_pica.h"
#include "../src/hle/hle.h"
#include "n64pass_shbin.h"

#define TW 512
#define TH 256
#define MAX_TARGETS 6
#define VBUF_VERTS (3 * 32768)  // a frame's vertices (a frame that needs more ends early)
#define MAX_TEX 512  // Mario Kart 64's title makes ~154 new textures a frame (its sky scrolls: job 1470)

typedef struct { float pos[4]; u8 col[4]; float tc[2]; } PV;  // the shader's v0, v1, v2

typedef struct {
    u32 addr, width;
    C3D_Tex tex;
    C3D_RenderTarget* rt;
    u32 lastUse;
    bool live;
} Target;

typedef struct {
    u64 key;
    C3D_Tex tex;
    u16 w, h;
    u32 lastUse;
    bool live;
} TexEntry;

// What a batch of triangles shares; a change ends the batch (the draw call)
typedef struct {
    Target* target;
    u64 combine, om;
    u32 cyc, flags;               // flags as the HLE's (4 shade, 2 texture, 1 z), 8 rectangle
    u32 prim, env, fog, blend, primLod, primZ;
    TexEntry* tex[2];
    float tc[2][4];
    u32 sxh, syh, sxl, syl;
} Batch;

static struct {
    bool ready;
    DVLB_s* dvlb;
    shaderProgram_s prog;
    int uTc0, uTc1;
    C3D_RenderTarget* screen;
    Target targets[MAX_TARGETS];
    TexEntry tex[MAX_TEX];
    PV* vbuf;
    u32 used, batchStart;
    bool frameOpen;
    Batch cur;
    bool curValid;
    u32 frame;
    u32* capBuf;  // a target read back for relay pictures
} g;

RdpPicaStats g_picaStats;
int g_picaTexDumpN, g_picaLogN;
void (*g_picaLog)(const char* line);
void (*g_picaTexDump)(const u32* rgba, u32 w, u32 h, int tile);
static u8 g_morton[64];

// ---- Frames and targets -------------------------------------------------------------------------------------------
static void frame_begin(void) {
    if (g.frameOpen) return;
    C3D_FrameBegin(0);  // no vblank pacing (C3D_FRAME_SYNCDRAW): the emulator runs slower than the display
    g.frameOpen = true;
    g.used = g.batchStart = 0;
    g.curValid = false;
}

static Target* target_for(u32 addr, u32 width) {
    Target* free = NULL;
    Target* lru = NULL;
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target* t = &g.targets[i];
        if (t->live && t->addr == addr) { t->width = width; t->lastUse = g.frame; return t; }
        if (!t->live && !free) free = t;
        if (t->live && (!lru || t->lastUse < lru->lastUse)) lru = t;
    }
    Target* t = free ? free : lru;
    if (!t->rt) {
        if (!C3D_TexInitVRAM(&t->tex, TW, TH, GPU_RGBA8)) return NULL;
        t->rt = C3D_RenderTargetCreateFromTex(&t->tex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
        if (!t->rt) return NULL;
    }
    t->addr = addr;
    t->width = width;
    t->live = true;
    t->lastUse = g.frame;
    C3D_RenderTargetClear(t->rt, C3D_CLEAR_ALL, 0x000000FF, 0);
    return t;
}

// ---- Textures -----------------------------------------------------------------------------------------------------
static u32 pow2(u32 v) { u32 p = 8; while (p < v && p < 1024) p <<= 1; return p; }

static u64 hash_bytes(u64 h, const u8* p, u32 n) {  // a word at a time where it can (TMEM runs are 8-byte aligned)
    g_picaStats.hashBytes += n;
    u32 i = 0;
    for (; i + 4 <= n; i += 4) { u32 w; memcpy(&w, p + i, 4); h = (h ^ w) * 0x100000001b3ull; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static u32* g_scratch;  // a decoded tile's rows (up to 1024 x 1024)
static u32 g_scratchN;

// The texture of tile `tile` as the current state describes it, decoded and cached. sizeOut: its width / height.
static struct { u32 gen; u64 key; TexEntry* e; } g_tileMemo[8];  // each tile's last lookup, valid while g_rdpTexGen holds

static TexEntry* fetch_tex_(int tile);
static TexEntry* fetch_tex(int tile) {
    typeof(g_tileMemo[0])* m = &g_tileMemo[tile & 7];
    if (m->e && m->gen == g_rdpTexGen && m->e->live && m->e->key == m->key) { m->e->lastUse = g.frame; return m->e; }
    u64 t0 = svcGetSystemTick();
    TexEntry* e = fetch_tex_(tile);
    g_picaStats.lookupTicks += svcGetSystemTick() - t0;
    if (e) { m->gen = g_rdpTexGen; m->key = e->key; m->e = e; }
    return e;
}

static TexEntry* fetch_tex_(int tile) {
    const RdpTile* t = &g_rdp.tiles[tile & 7];
    u32 tw = ((t->sh - t->sl) >> 2) + 1, th = ((t->th - t->tl) >> 2) + 1;
    if (t->masks && !(t->cms & 2)) tw = 1u << t->masks;
    if (t->maskt && !(t->cmt & 2)) th = 1u << t->maskt;
    if (tw > 1024) tw = 1024;
    if (th > 1024) th = 1024;
    u32 W = pow2(tw), H = pow2(th);
    // What the texels come from: the tile, the TMEM bytes it covers, the palette and the TLUT modes
    u64 h = 0xcbf29ce484222325ull;
    h = hash_bytes(h, (const u8*)t, sizeof *t);
    u32 om = RDP_OM(45, 3);  // filter, TLUT enable, TLUT type
    h = hash_bytes(h, (const u8*)&om, 4);
    u32 start = t->tmem * 8, bytes = t->line * 8 * th;
    if (t->size == 3 || bytes > 4096 || !bytes) bytes = 4096, start = 0;
    for (u32 i = 0; i < bytes; i += 512) {
        u32 n = bytes - i < 512 ? bytes - i : 512, a = (start + i) & 0xFFF;
        if (a + n > 4096) { h = hash_bytes(h, g_rdp.tmem + a, 4096 - a); h = hash_bytes(h, g_rdp.tmem, n - (4096 - a)); }
        else h = hash_bytes(h, g_rdp.tmem + a, n);
    }
    if (t->fmt == 2 || RDP_OM(47, 1)) h = hash_bytes(h, g_rdp.tmem + 0x800, 0x800);
    // A hit; else the oldest entry of the same size not used this frame (its memory is reused as it is), else a free
    // one, else the oldest (freed and allocated again: slow)
    TexEntry *lru = NULL, *sameSize = NULL, *freeOne = NULL;
    for (int i = 0; i < MAX_TEX; i++) {
        TexEntry* e = &g.tex[i];
        if (e->live && e->key == h) { e->lastUse = g.frame; return e; }
        if (!e->live) { if (!freeOne) freeOne = e; continue; }
        if (e->lastUse != g.frame && e->w == W && e->h == H && (!sameSize || e->lastUse < sameSize->lastUse)) sameSize = e;
        if (!lru || e->lastUse < lru->lastUse) lru = e;
    }
    TexEntry* e = sameSize ? sameSize : freeOne ? freeOne : lru;
    if (e->live && (e->w != W || e->h != H)) { C3D_TexDelete(&e->tex); e->live = false; }
    if (!e->live) {
        if (!C3D_TexInit(&e->tex, (u16)W, (u16)H, GPU_RGBA8)) return NULL;
        g_picaStats.texAllocs++;
    }
    e->live = true;
    e->key = h;
    e->w = (u16)W;
    e->h = (u16)H;
    e->lastUse = g.frame;
    if (g_scratchN < tw * th) {
        free(g_scratch);
        g_scratch = malloc(tw * th * 4);
        g_scratchN = g_scratch ? tw * th : 0;
        if (!g_scratch) return NULL;
    }
    rdp_decode_tile(tile, tw, th, g_scratch);
    g_picaStats.texels += W * H;
    u32* dst = (u32*)e->tex.data;
    for (u32 y = 0; y < H; y++) {
        const u32* row = g_scratch + (y < th ? y : th - 1) * tw;  // beyond the tile: its last row (clamped tiles)
        u32* tiles = dst + (y >> 3) * (W >> 3) * 64;  // row 0 at t = 1, as the PICA samples and draws (jobs 1458-1459)
        const u8* m = &g_morton[(y & 7) * 8];
        for (u32 x = 0; x < W; x++) tiles[(x >> 3) * 64 + m[x & 7]] = row[x < tw ? x : tw - 1];
    }
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)e->tex.data, e->tex.size);  // (C3D_TexFlush: an IPC call to GSP)
    if (g_picaTexDumpN > 0 && g_picaTexDump) {  // debugging: the decoded texture as the frontend wants it
        u32* lin = malloc(W * H * 4);
        if (lin) {
            for (u32 y = 0; y < H; y++)
                for (u32 x = 0; x < W; x++)
                    lin[y * W + x] = rdp_texel(tile, (int)(x < tw ? x : tw - 1), (int)(y < th ? y : th - 1));
            g_picaTexDump(lin, W, H, tile);
            free(lin);
            g_picaTexDumpN--;
        }
    }
    GPU_TEXTURE_FILTER_PARAM f = RDP_OM(45, 1) ? GPU_LINEAR : GPU_NEAREST;
    C3D_TexSetFilter(&e->tex, f, f);
    GPU_TEXTURE_WRAP_PARAM ws = (t->cms & 2) || !t->masks ? GPU_CLAMP_TO_EDGE : (t->cms & 1) ? GPU_MIRRORED_REPEAT : GPU_REPEAT;
    GPU_TEXTURE_WRAP_PARAM wt = (t->cmt & 2) || !t->maskt ? GPU_CLAMP_TO_EDGE : (t->cmt & 1) ? GPU_MIRRORED_REPEAT : GPU_REPEAT;
    C3D_TexSetWrap(&e->tex, ws, wt);
    g_picaStats.texDecodes++;
    return e;
}

// Texels (s, t in texels, 10.5 / 32) to a texture unit's coordinates for tile `tile`: shift, the tile's origin,
// the size it was decoded at; t runs up the PICA texture
static void tex_coords(int tile, const TexEntry* e, float out[4]) {
    const RdpTile* t = &g_rdp.tiles[tile & 7];
    float ss = t->shifts && t->shifts <= 10 ? 1.0f / (float)(1 << t->shifts) : t->shifts > 10 ? (float)(1 << (16 - t->shifts)) : 1.0f;
    float st = t->shiftt && t->shiftt <= 10 ? 1.0f / (float)(1 << t->shiftt) : t->shiftt > 10 ? (float)(1 << (16 - t->shiftt)) : 1.0f;
    float W = e ? e->w : 8, H = e ? e->h : 8;
    out[0] = ss / W;
    out[1] = -st / H;
    out[2] = -(float)t->sl / 4.0f / W;
    out[3] = 1.0f + (float)t->tl / 4.0f / H;
}

// ---- State --------------------------------------------------------------------------------------------------------
static u32 abgr(u32 rgba) {  // RGBA8888 (red on top) to citro3d's colour order
    return ((rgba & 0xFF) << 24) | (((rgba >> 8) & 0xFF) << 16) | (((rgba >> 16) & 0xFF) << 8) | (rgba >> 24);
}

static u32 konst(int k, const Batch* b, bool alpha) {
    u32 p = b->prim, e = b->env;
    switch (k) {
    case PK_ONE: return 0xFFFFFFFF;
    case PK_PRIM: return p;
    case PK_ENV: return e;
    case PK_PRIM_ALPHA: return (p & 0xFF) * 0x01010101u;
    case PK_ENV_ALPHA: return (e & 0xFF) * 0x01010101u;
    case PK_PRIM_LOD: return (b->primLod & 0xFF) * 0x01010101u;
    default: return alpha ? 0 : 0;
    }
}

static void apply_tev(const Batch* b) {
    TevProgram p;
    if (b->flags & 16) {  // a fill: the colour as the constant
        memset(&p, 0, sizeof p);
        p.n = 1;
        p.st[0].srcRgb[0] = p.st[0].srcA[0] = PS_CONSTANT;
        p.st[0].constRgb = p.st[0].constA = PK_PRIM;
    } else if (b->cyc == RDP_COPY) {  // copy mode: the texel as it is
        memset(&p, 0, sizeof p);
        p.n = 1;
        p.st[0].srcRgb[0] = p.st[0].srcA[0] = PS_TEX0;
    } else {
        cc_pica_compile(b->combine, b->cyc, &p);
    }
    // Shade off: the triangle's colour is the primitive colour (vertex colours were set to it)
    u32 bufMaskRgb = 0, bufMaskA = 0;
    for (int i = 0; i < 6; i++) {
        C3D_TexEnv* env = C3D_GetTexEnv(i);
        C3D_TexEnvInit(env);
        if (i >= p.n) continue;
        const TevStage* s = &p.st[i];
        C3D_TexEnvSrc(env, C3D_RGB, (GPU_TEVSRC)s->srcRgb[0], (GPU_TEVSRC)s->srcRgb[1], (GPU_TEVSRC)s->srcRgb[2]);
        C3D_TexEnvSrc(env, C3D_Alpha, (GPU_TEVSRC)s->srcA[0], (GPU_TEVSRC)s->srcA[1], (GPU_TEVSRC)s->srcA[2]);
        C3D_TexEnvOpRgb(env, (GPU_TEVOP_RGB)s->opRgb[0], (GPU_TEVOP_RGB)s->opRgb[1], (GPU_TEVOP_RGB)s->opRgb[2]);
        C3D_TexEnvOpAlpha(env, (GPU_TEVOP_A)s->opA[0], (GPU_TEVOP_A)s->opA[1], (GPU_TEVOP_A)s->opA[2]);
        C3D_TexEnvFunc(env, C3D_RGB, (GPU_COMBINEFUNC)s->funcRgb);
        C3D_TexEnvFunc(env, C3D_Alpha, (GPU_COMBINEFUNC)s->funcA);
        u32 c = (konst(s->constRgb, b, false) & 0xFFFFFF00) | (konst(s->constA, b, true) & 0xFF);
        if (b->flags & 16) c = b->prim;
        C3D_TexEnvColor(env, abgr(c));
        if (i < 4 && s->bufRgb) bufMaskRgb |= 1u << i;
        if (i < 4 && s->bufA) bufMaskA |= 1u << i;
    }
    C3D_TexEnvBufUpdate(C3D_RGB, bufMaskRgb);
    C3D_TexEnvBufUpdate(C3D_Alpha, bufMaskA);
    u32 buf = (konst(p.globalRgb, b, false) & 0xFFFFFF00) | (konst(p.globalA, b, true) & 0xFF);
    C3D_TexEnvBufColor(abgr(buf));
    if (p.approx) g_picaStats.approxDraws++;
}

static void apply_state(const Batch* b) {
    C3D_FrameDrawOn(b->target->rt);
    C3D_BindProgram(&g.prog);
    apply_tev(b);
    for (int u = 0; u < 2; u++) {
        if (b->tex[u]) C3D_TexBind(u, &b->tex[u]->tex);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, u ? g.uTc1 : g.uTc0, b->tc[u][0], b->tc[u][1], b->tc[u][2], b->tc[u][3]);
    }
    // Depth: N64 smaller is nearer; the PICA here keeps 1 - z, so nearer is greater
    bool rect = b->flags & 8;
    bool zcmp = !rect && (b->flags & 1) && RDP_OM(4, 1), zupd = !rect && (b->flags & 1) && RDP_OM(5, 1);
    u32 zmode = RDP_OM(10, 2);
    GPU_WRITEMASK wm = GPU_WRITE_COLOR | (zupd ? GPU_WRITE_DEPTH : 0);
    if (b->flags & 32) wm = GPU_WRITE_DEPTH;  // a depth clear
    C3D_DepthTest(zcmp || (b->flags & 32), (b->flags & 32) ? GPU_ALWAYS : GPU_GEQUAL, wm);
    C3D_DepthMap(true, -1.0f, zmode == 3 ? 0.0005f : 0.0f);  // decal: a little nearer
    C3D_CullFace(GPU_CULL_NONE);
    // Alpha compare against the blend colour's alpha
    if (RDP_OM(0, 1) && b->cyc < RDP_COPY) C3D_AlphaTest(true, GPU_GREATER, (int)(b->blend & 0xFF));
    else if (b->cyc == RDP_COPY && RDP_OM(0, 1)) C3D_AlphaTest(true, GPU_GREATER, 0);
    else C3D_AlphaTest(false, GPU_ALWAYS, 0);
    // Blending: force-blend with (pixel x its alpha + memory x (1 - alpha)) as the last cycle; otherwise opaque
    int lastShift = b->cyc == RDP_CYC2 ? 16 : 18;
    u32 pa = RDP_OM(lastShift + 12, 2), aa = RDP_OM(lastShift + 8, 2), ma = RDP_OM(lastShift + 4, 2), ba = RDP_OM(lastShift, 2);
    if (b->cyc < RDP_COPY && RDP_OM(14, 1) && pa == 0 && ma == 1 && ba == 0 && aa != 3)
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    else
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    // The scissor, in the target's pixels (its rows run up from the bottom)
    u32 x0 = b->sxh >> 2, x1 = b->sxl >> 2, y0 = b->syh >> 2, y1 = b->syl >> 2;
    if (x1 > TW) x1 = TW;
    if (y1 > TH) y1 = TH;
    if (x0 < x1 && y0 < y1) C3D_SetScissor(GPU_SCISSOR_NORMAL, x0, TH - y1, x1, TH - y0);
    else C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

static void flush(void) {
    u32 n = g.used - g.batchStart;
    if (!n || !g.curValid) { g.batchStart = g.used; return; }
    u64 t0 = svcGetSystemTick();
    apply_state(&g.cur);
    C3D_DrawArrays(GPU_TRIANGLES, (int)g.batchStart, (int)n);
    g_picaStats.drawCalls++;
    g_picaStats.drawTicks += svcGetSystemTick() - t0;
    g.batchStart = g.used;
}

static bool same_batch(const Batch* a, const Batch* b) { return memcmp(a, b, sizeof *a) == 0; }

// The state for a draw: begins a new batch when it differs from the open one. False: nowhere to draw.
static bool begin_draw(u32 flags, u32 verts) {
    frame_begin();
    if (g.used + verts > VBUF_VERTS) {  // the frame's vertex buffer is full: draw what is there and start a frame
        flush();
        C3D_FrameEnd(0);
        g.frameOpen = false;
        frame_begin();
        g_picaStats.splits++;
    }
    Batch b;
    memset(&b, 0, sizeof b);
    b.target = target_for(g_rdp.ciAddr & 0xFFFFFF, g_rdp.ciWidth);
    if (!b.target) return false;
    b.cyc = RDP_OM(52, 2);
    b.flags = flags;
    b.om = g_rdp.om;
    b.combine = g_rdp.combine;
    b.prim = g_rdp.prim; b.env = g_rdp.env; b.fog = g_rdp.fog; b.blend = g_rdp.blend;
    b.primLod = g_rdp.primLod; b.primZ = g_rdp.primZ;
    b.sxh = g_rdp.sxh; b.syh = g_rdp.syh; b.sxl = g_rdp.sxl; b.syl = g_rdp.syl;
    if ((flags & 2) && !(flags & 16)) {
        int tile = (int)(flags >> 8) & 7;
        for (int u = 0; u < 2; u++) {
            b.tex[u] = fetch_tex(tile + u);
            tex_coords(tile + u, b.tex[u], b.tc[u]);
        }
    }
    if (!g.curValid || !same_batch(&b, &g.cur)) {
        flush();
        g.cur = b;
        g.curValid = true;
    }
    return true;
}

static void put(float sx, float sy, float sz, float w, u32 rgba, float s, float t) {
    PV* v = &g.vbuf[g.used++];
    // Screen pixels to the target's clip space (x right, y up from the bottom row), depth kept as 1 - z
    v->pos[0] = (sx / (TW / 2.0f) - 1.0f) * w;
    v->pos[1] = (1.0f - sy / (TH / 2.0f)) * w;
    v->pos[2] = -(1.0f - sz) * w;
    v->pos[3] = w;
    v->col[0] = (u8)(rgba >> 24); v->col[1] = (u8)(rgba >> 16); v->col[2] = (u8)(rgba >> 8); v->col[3] = (u8)rgba;
    v->tc[0] = s;
    v->tc[1] = t;
}

static u32 to_rgba(float r, float g2, float b, float a) {
    int R = (int)r, G = (int)g2, B = (int)b, A = (int)a;
    R = R < 0 ? 0 : R > 255 ? 255 : R; G = G < 0 ? 0 : G > 255 ? 255 : G;
    B = B < 0 ? 0 : B > 255 ? 255 : B; A = A < 0 ? 0 : A > 255 ? 255 : A;
    return (u32)R << 24 | (u32)G << 16 | (u32)B << 8 | (u32)A;
}

// ---- What the HLE and the software RDP hand over ------------------------------------------------------------------
static void on_polygon(const HleVtx* v, int n, u32 flags) {
    if (n < 3 || RDP_OM(52, 2) == RDP_FILL) return;  // triangles in fill mode draw nothing useful
    if (!begin_draw(flags & 0x707, (u32)(n - 2) * 3)) return;
    if (g_picaLogN > 0 && g_picaLog && (flags & 2)) {  // debugging: what a textured draw sends
        char line[320];
        const RdpTile* t = &g_rdp.tiles[(flags >> 8) & 7];
        snprintf(line, sizeof line, "# draw tile %u fmt %u size %u line %u sl %u tl %u sh %u th %u mask %u %u shift %u %u cm %u %u "
                 "tex %ux%u tc %.4f %.4f %.4f %.4f | v0 %.1f %.1f s %.1f t %.1f | v1 %.1f %.1f s %.1f t %.1f | v2 %.1f %.1f s %.1f t %.1f\n",
                 (unsigned)((flags >> 8) & 7), (unsigned)t->fmt, (unsigned)t->size, (unsigned)t->line, (unsigned)t->sl,
                 (unsigned)t->tl, (unsigned)t->sh, (unsigned)t->th, (unsigned)t->masks, (unsigned)t->maskt,
                 (unsigned)t->shifts, (unsigned)t->shiftt, (unsigned)t->cms, (unsigned)t->cmt,
                 g.cur.tex[0] ? (unsigned)g.cur.tex[0]->w : 0, g.cur.tex[0] ? (unsigned)g.cur.tex[0]->h : 0,
                 g.cur.tc[0][0], g.cur.tc[0][1], g.cur.tc[0][2], g.cur.tc[0][3], v[0].sx, v[0].sy, v[0].s / 32, v[0].t / 32,
                 v[1].sx, v[1].sy, v[1].s / 32, v[1].t / 32, v[2].sx, v[2].sy, v[2].s / 32, v[2].t / 32);
        g_picaLog(line);
        g_picaLogN--;
    }
    bool persp = RDP_OM(51, 1);
    float primZ = (float)(g_rdp.primZ >> 1) / 32767.0f;
    for (int k = 1; k + 1 < n; k++) {
        const HleVtx* tri[3] = {&v[0], &v[k], &v[k + 1]};
        for (int j = 0; j < 3; j++) {
            const HleVtx* p = tri[j];
            u32 c = (flags & 4) ? to_rgba(p->r, p->g, p->b, p->a) : g_rdp.prim;
            float z = RDP_OM(2, 1) ? primZ : p->sz;
            put(p->sx, p->sy, z, persp ? p->w : 1.0f, c, p->s / 32.0f, p->t / 32.0f);
        }
    }
    g_picaStats.triangles += (u32)(n - 2);
}

static void quad(float x0, float y0, float x1, float y1, u32 rgba, float s0, float t0, float s1, float t1, bool flip, float z) {
    float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    float ss[4] = {s0, s1, s1, s0}, ts[4] = {t0, t0, t1, t1};
    if (flip) { ss[1] = s0; ss[3] = s1; ts[1] = t1; ts[3] = t0; }
    static const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; k++) {
        int i = order[k];
        put(xs[i], ys[i], z, 1.0f, rgba, ss[i], ts[i]);
    }
}

static u32 fill_rgba(void) {  // the fill colour as RGBA8888 (16-bit images: the first pixel's 5551)
    if (g_rdp.ciSize == 3) return g_rdp.fill;
    u16 p = (u16)(g_rdp.fill >> 16);
    u32 r = (p >> 11) & 31, gg = (p >> 6) & 31, b = (p >> 1) & 31;
    return (r << 27 | (r >> 2) << 24) | (gg << 19 | (gg >> 2) << 16) | (b << 11 | (b >> 2) << 8) | 0xFF;
}

static void on_fill_rect(u64 w) {
    float xl = (float)((w >> 44) & 0xFFF) / 4, yl = (float)((w >> 32) & 0xFFF) / 4;
    float xh = (float)((w >> 12) & 0xFFF) / 4, yh = (float)(w & 0xFFF) / 4;
    u32 cyc = RDP_OM(52, 2);
    if (cyc == RDP_FILL || cyc == RDP_COPY) { xl += 1; yl += 1; }
    bool depth = (g_rdp.ciAddr & 0xFFFFFF) == (g_rdp.zAddr & 0xFFFFFF);
    if (depth) {
        // The game clearing its Z buffer: the depth of the target it draws with next (the colour image before it)
        Target* t = NULL;
        for (int i = 0; i < MAX_TARGETS; i++)
            if (g.targets[i].live && (!t || g.targets[i].lastUse >= t->lastUse) && g.targets[i].addr != (g_rdp.zAddr & 0xFFFFFF))
                t = &g.targets[i];
        if (!t) return;
        frame_begin();
        flush();
        g.curValid = false;
        Batch b;
        memset(&b, 0, sizeof b);
        b.target = t;
        b.flags = 8 | 16 | 32;
        b.cyc = RDP_FILL;
        b.sxh = g_rdp.sxh; b.syh = g_rdp.syh; b.sxl = g_rdp.sxl; b.syl = g_rdp.syl;
        g.cur = b;
        g.curValid = true;
        quad(xh, yh, xl, yl, 0, 0, 0, 0, 0, false, 1.0f);  // the far value (stored as 1 - z: 0)
        flush();
        g.curValid = false;
        g_picaStats.depthClears++;
        return;
    }
    if (cyc == RDP_FILL) {
        u32 saved = g_rdp.prim;
        g_rdp.prim = fill_rgba();
        if (!begin_draw(8 | 16, 6)) { g_rdp.prim = saved; return; }
        quad(xh, yh, xl, yl, g_rdp.prim, 0, 0, 0, 0, false, 0.0f);
        g_rdp.prim = saved;
    } else {
        if (!begin_draw(8, 6)) return;
        quad(xh, yh, xl, yl, g_rdp.prim, 0, 0, 0, 0, false, 0.0f);
    }
    g_picaStats.rects++;
}

static void on_tex_rect(const u64* w, bool flip) {
    float xl = (float)((w[0] >> 44) & 0xFFF) / 4, yl = (float)((w[0] >> 32) & 0xFFF) / 4;
    float xh = (float)((w[0] >> 12) & 0xFFF) / 4, yh = (float)(w[0] & 0xFFF) / 4;
    int tile = (int)((w[0] >> 24) & 7);
    float s0 = (float)(s16)(w[1] >> 48) / 32.0f, t0 = (float)(s16)(w[1] >> 32) / 32.0f;
    float dsdx = (float)(s16)(w[1] >> 16) / 1024.0f, dtdy = (float)(s16)w[1] / 1024.0f;
    u32 cyc = RDP_OM(52, 2);
    if (cyc == RDP_COPY) dsdx /= 4;
    if (cyc == RDP_FILL || cyc == RDP_COPY) { xl += 1; yl += 1; }
    if (!begin_draw(8 | 2 | (u32)tile << 8, 6)) return;
    float s1 = s0 + dsdx * (flip ? (yl - yh) : (xl - xh)), t1 = t0 + dtdy * (flip ? (xl - xh) : (yl - yh));
    quad(xh, yh, xl, yl, g_rdp.prim, s0, t0, s1, t1, flip, 0.0f);
    g_picaStats.rects++;
}

static void on_triangle(const u64* w, int n) {  // from microcodes run on the interpreter: not drawn yet
    (void)w; (void)n;
    g_picaStats.lleTriangles++;
}

static const RdpGpu kGpu = {on_fill_rect, on_tex_rect, on_triangle};

// ---- Presenting ---------------------------------------------------------------------------------------------------
static bool present(u32 origin, u32 width, u32 height);
bool rdp_pica_present(u32 origin, u32 width, u32 height) {  // (the profiler's phase around it)
    u8 phase = g_phase;
    g_phase = PH_GPU;
    bool r = present(origin, width, height);
    g_phase = phase;
    return r;
}
static bool present(u32 origin, u32 width, u32 height) {
    if (!g.ready) return false;
    Target* show = NULL;
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target* t = &g.targets[i];
        if (t->live && origin >= t->addr && origin < t->addr + t->width * 2 * 8) { show = t; break; }
    }
    if (!show) {
        if (g.frameOpen) { flush(); C3D_FrameEnd(0); g.frameOpen = false; }
        return false;
    }
    frame_begin();
    flush();
    g.curValid = false;
    C3D_FrameDrawOn(g.screen);
    C3D_BindProgram(&g.prog);
    C3D_TexBind(0, &show->tex);
    C3D_TexSetFilter(&show->tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexEnv* e = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(e, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvColor(e, 0xFFFFFFFF);
    for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uTc0, 1.0f / TW, -1.0f / TH, 0.0f, 1.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uTc1, 0, 0, 0, 0);
    // The picture centred on the 400 x 240 top screen (halved when larger): screen pixels to the rotated target
    u32 outW = width, outH = height;
    if (outW > 400 || outH > 240) { outW = width / 2; outH = height / 2; }
    float x0 = (400.0f - (float)outW) / 2, y0 = (240.0f - (float)outH) / 2;
    C3D_Mtx proj;
    Mtx_OrthoTilt(&proj, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
    float xs[4] = {x0, x0 + outW, x0 + outW, x0}, ys[4] = {y0, y0, y0 + outH, y0 + outH};
    float ss[4] = {0, (float)width, (float)width, 0}, ts[4] = {0, 0, (float)height, (float)height};
    static const int order[6] = {0, 1, 2, 0, 2, 3};
    u32 first = g.used;
    for (int k = 0; k < 6; k++) {
        int i = order[k];
        C3D_FVec p = Mtx_MultiplyFVec4(&proj, FVec4_New(xs[i], ys[i], 0.5f, 1.0f));
        PV* v = &g.vbuf[g.used++];
        v->pos[0] = p.x; v->pos[1] = p.y; v->pos[2] = p.z; v->pos[3] = p.w;
        v->col[0] = v->col[1] = v->col[2] = v->col[3] = 255;
        v->tc[0] = ss[i]; v->tc[1] = ts[i];
    }
    C3D_DrawArrays(GPU_TRIANGLES, (int)first, 6);
    g.batchStart = g.used;
    // A copy for the relay's pictures when one is wanted
    if (g.capBuf) {
        C3D_FrameSplit(0);  // the draws so far go to the GPU before the copy
        GX_TextureCopy((u32*)show->tex.data, 0, g.capBuf, 0, TW * TH * 4, GX_TRANSFER_RAW_COPY(1));  // tiled, as it is
    }
    C3D_FrameEnd(0);
    g.frameOpen = false;
    g.frame++;
    g_picaStats.presents++;
    return true;
}

// The last presented target as RGB888 rows (top first), for relay pictures; false when there is none
bool rdp_pica_capture(u8* rgb, u32 width, u32 height) {
    if (!g.capBuf) {
        g.capBuf = linearAlloc(TW * TH * 4);
        return false;  // from the next present on
    }
    // The copy queued with the last present done: a frame begun waits for the GPU to finish everything before it.
    // (gspWaitForPPF here could wait forever for an interrupt citro3d's own frame end had already taken.)
    if (g.frameOpen) { flush(); C3D_FrameEnd(0); g.frameOpen = false; }
    frame_begin();
    GSPGPU_InvalidateDataCache(g.capBuf, TW * TH * 4);
    for (u32 y = 0; y < height && y < TH; y++)
        for (u32 x = 0; x < width && x < TW; x++) {
            // De-tiled the way textures are tiled (fetch_tex): what sampling the target as a texture sees
            u32 r = y, p = g.capBuf[(r >> 3) * (TW >> 3) * 64 + (x >> 3) * 64 + g_morton[(r & 7) * 8 + (x & 7)]];
            u8* o = rgb + (y * width + x) * 3;
            o[0] = (u8)(p >> 24); o[1] = (u8)(p >> 16); o[2] = (u8)(p >> 8);
        }
    return true;
}

// ---- Set-up -------------------------------------------------------------------------------------------------------
bool rdp_pica_init(void) {
    if (g.ready) return true;
    for (u32 y = 0; y < 8; y++)
        for (u32 x = 0; x < 8; x++)
            g_morton[y * 8 + x] = (u8)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4)) return false;
    g.dvlb = DVLB_ParseFile((u32*)n64pass_shbin, n64pass_shbin_size);
    if (!g.dvlb) return false;
    shaderProgramInit(&g.prog);
    shaderProgramSetVsh(&g.prog, &g.dvlb->DVLE[0]);
    g.uTc0 = shaderInstanceGetUniformLocation(g.prog.vertexShader, "tc0");
    g.uTc1 = shaderInstanceGetUniformLocation(g.prog.vertexShader, "tc1");
    g.vbuf = linearAlloc(VBUF_VERTS * sizeof(PV) + 64 * sizeof(PV));
    if (!g.vbuf) return false;
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, g.vbuf, sizeof(PV), 3, 0x210);
    g.screen = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!g.screen) return false;
    C3D_RenderTargetSetOutput(g.screen, GFX_TOP, GFX_LEFT,
                              GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                              GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
                              GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    g.ready = true;
    return true;
}

void rdp_pica_attach(bool on) {
    g_rdpGpu = on && g.ready ? &kGpu : NULL;
    g_hleVtxSink = on && g.ready ? on_polygon : NULL;
}

void rdp_pica_reset(void) {
    if (!g.ready) return;
    if (g.frameOpen) { C3D_FrameEnd(0); g.frameOpen = false; }
    for (int i = 0; i < MAX_TARGETS; i++) g.targets[i].live = false;
    for (int i = 0; i < MAX_TEX; i++) if (g.tex[i].live) { C3D_TexDelete(&g.tex[i].tex); g.tex[i].live = false; }
    memset(g_tileMemo, 0, sizeof g_tileMemo);
    g.curValid = false;
    memset(&g_picaStats, 0, sizeof g_picaStats);
}
