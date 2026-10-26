// High-level graphics microcode: the display list of a graphics task is interpreted natively and turned into RDP
// commands (rdp_command), instead of running the microcode on the RSP interpreter. Three GBI dialects: F3D (Fast3D,
// "RSP SW Version 2.0x": Super Mario 64), F3DEX 0.95-1.x (Mario Kart 64, Star Fox 64) and F3DEX2 / F3DZEX 2.x
// (Ocarina of Time, Paper Mario). Other microcodes run on the interpreter. Arithmetic is in float: the pictures are
// checked against the interpreter path's (core/tools, DESIGN.md "Exactness model"), not bit for bit yet.
// Layouts and opcodes as in libultra's gbi.h and the GBI notes on n64brew.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sys/sys.h"
#include "hle.h"

void rdp_command(const u64* words, int n);
// The renderer's time is counted as the RDP's (g_timeRdp), so the statistics split geometry from rasterization
bool g_hleTimeRdp = true;  // per-command timing (a host tick each side: the 3DS turns it off, the tick is a syscall)
static void rdp_out(const u64* w, int n) {
    if (!g_hleTimeRdp) { rdp_command(w, n); return; }
    u64 t0 = host_ticks();
    rdp_command(w, n);
    g_timeRdp += host_ticks() - t0;
}
void rdp_full_sync(void);

enum { UC_NONE, UC_F3D, UC_F3DEX, UC_F3DEX2 };

typedef struct {
    float x, y, z, w;           // clip space
    float sx, sy, sz;           // screen (pixels, depth 0..1)
    float s, t;                 // texture coordinates, 10.5 texel units already scaled
    float r, g, b, a;           // 0..255
    u8 clip;                    // outside flags
} V;

typedef struct { float m[4][4]; } M4;

static struct {
    int uc;
    u32 seg[16];
    M4 mv[18], proj, mvp;
    int mvDepth;
    bool mvpDirty;
    V vtx[64];
    u32 geom;
    float vscale[4], vtrans[4];
    u8 lightCol[8][3];
    float lightDir[8][3];
    int numLights;
    float lookat[2][3];
    u32 texOn, texTile, texScaleS, texScaleT;
    s16 fogMul, fogOff;
    u32 half1, half2;
    u64 omH, omL;               // other modes (the RDP word is rebuilt from both halves)
    u32 textHash;
    bool fullSync;
    u64 cmds;
    int triDiv;                 // Fast3D triangle indices: x10, or x5 in Wave Race's variant
    u32 dl;                     // the task's display list (OSTask data_ptr, read by hle_gfx_prepare)
    u64 taskTris;               // triangles the task drew (its estimated run time: src/sys/sp.c)
} G;

HleStats g_hleStats;
bool g_hleGfx;
void (*g_hleVtxSink)(const HleVtx* v, int n, u32 flags);
const HleAsync* g_hleAsync;
bool g_hleAhead;
static bool g_hleTrace;     // N64_HLE_TRACE=task: every command of that graphics task (0 = the first)
static u64 g_hleTraceTask;

// Geometry mode bits by dialect
static u32 GM_ZBUF, GM_SHADE, GM_SMOOTH, GM_CULLF, GM_CULLB, GM_FOG, GM_LIGHT, GM_TGEN, GM_TGENL;
static void set_dialect(int uc) {
    G.uc = uc;
    GM_ZBUF = 0x1; GM_SHADE = 0x4; GM_FOG = 0x10000; GM_LIGHT = 0x20000; GM_TGEN = 0x40000; GM_TGENL = 0x80000;
    if (uc == UC_F3DEX2) { GM_CULLF = 0x200; GM_CULLB = 0x400; GM_SMOOTH = 0x200000; }
    else { GM_CULLF = 0x1000; GM_CULLB = 0x2000; GM_SMOOTH = 0x200; }
}

// ---- Memory -------------------------------------------------------------------------------------------------------
static inline u32 seg_addr(u32 a) { return (G.seg[(a >> 24) & 15] + (a & 0xFFFFFF)) & 0xFFFFFF; }
static inline u32 rd32(u32 a) { return m_r32(g_mem.rdram, a & (g_mem.rdramSize - 1) & ~3u); }
static inline u16 rd16(u32 a) { return m_r16(g_mem.rdram, a & (g_mem.rdramSize - 1) & ~1u); }
static inline u8 rd8(u32 a) { return m_r8(g_mem.rdram, a & (g_mem.rdramSize - 1)); }

// ---- Matrices -----------------------------------------------------------------------------------------------------
static void load_mtx(M4* o, u32 a) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            s16 hi = (s16)rd16(a + (u32)(i * 8 + j * 2));
            u16 lo = rd16(a + 32 + (u32)(i * 8 + j * 2));
            o->m[i][j] = (float)((s32)((u32)hi << 16 | lo)) / 65536.0f;
        }
}
static void mul(M4* o, const M4* a, const M4* b) {  // o = a * b (row vectors: v * a * b)
    M4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
    *o = r;
}

static void do_mtx(u32 a, bool proj, bool load, bool push) {
    M4 m;
    load_mtx(&m, a);
    if (g_hleTrace && g_hleStats.tasks == g_hleTraceTask)
        fprintf(stderr, "    mtx %06x %s%s%s depth %d: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.1f %.1f %.1f %.3f]\n",
                (unsigned)a, proj ? "proj" : "mv", load ? " load" : " mul", push ? " push" : "", G.mvDepth, m.m[0][0], m.m[0][1],
                m.m[0][2], m.m[0][3], m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3], m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3],
                m.m[3][0], m.m[3][1], m.m[3][2], m.m[3][3]);
    if (proj) {
        if (load) G.proj = m;
        else mul(&G.proj, &m, &G.proj);
    } else {
        if (push && G.mvDepth < 17) { G.mv[G.mvDepth + 1] = G.mv[G.mvDepth]; G.mvDepth++; }
        if (load) G.mv[G.mvDepth] = m;
        else mul(&G.mv[G.mvDepth], &m, &G.mv[G.mvDepth]);
    }
    G.mvpDirty = true;
}

static void update_mvp(void) {
    if (!G.mvpDirty) return;
    mul(&G.mvp, &G.mv[G.mvDepth], &G.proj);
    G.mvpDirty = false;
}

// ---- Vertices -----------------------------------------------------------------------------------------------------
static void load_vertices(u32 a, int v0, int n) {
    update_mvp();
    const M4* m = &G.mvp;
    const M4* mv = &G.mv[G.mvDepth];
    // Light and lookat directions in model space (the modelview transposed), normalized
    float lm[8][3], la[2][3];
    for (int l = 0; l < 10; l++) {
        const float* e = l < 8 ? G.lightDir[l] : G.lookat[l - 8];
        float* o = l < 8 ? lm[l] : la[l - 8];
        for (int i = 0; i < 3; i++) o[i] = mv->m[i][0] * e[0] + mv->m[i][1] * e[1] + mv->m[i][2] * e[2];
        float len = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        if (len > 0) { o[0] /= len; o[1] /= len; o[2] /= len; }
    }
    for (int k = 0; k < n && v0 + k < 64; k++, a += 16) {
        V* v = &G.vtx[v0 + k];
        float x = (s16)rd16(a), y = (s16)rd16(a + 2), z = (s16)rd16(a + 4);
        v->x = x * m->m[0][0] + y * m->m[1][0] + z * m->m[2][0] + m->m[3][0];
        v->y = x * m->m[0][1] + y * m->m[1][1] + z * m->m[2][1] + m->m[3][1];
        v->z = x * m->m[0][2] + y * m->m[1][2] + z * m->m[2][2] + m->m[3][2];
        v->w = x * m->m[0][3] + y * m->m[1][3] + z * m->m[2][3] + m->m[3][3];
        float s = (s16)rd16(a + 8), t = (s16)rd16(a + 10);
        u8 c0 = rd8(a + 12), c1 = rd8(a + 13), c2 = rd8(a + 14), c3 = rd8(a + 15);
        v->a = c3;
        if (G.geom & GM_LIGHT) {
            // As F3DEX2 does (its disassembly, Mr-Wiseguy/f3dex2): the raw normal (s8 / 128, not normalized) against
            // the light and lookat directions taken into model space (modelview transposed) and normalized there
            float nx = (s8)c0 / 128.0f, ny = (s8)c1 / 128.0f, nz = (s8)c2 / 128.0f;
            float r = G.lightCol[G.numLights][0], g = G.lightCol[G.numLights][1], b = G.lightCol[G.numLights][2];
            for (int l = 0; l < G.numLights; l++) {
                float d = nx * lm[l][0] + ny * lm[l][1] + nz * lm[l][2];
                if (d > 0) { r += G.lightCol[l][0] * d; g += G.lightCol[l][1] * d; b += G.lightCol[l][2] * d; }
            }
            v->r = r > 255 ? 255 : r; v->g = g > 255 ? 255 : g; v->b = b > 255 ? 255 : b;
            if (G.geom & GM_TGEN) {  // environment mapping: 0x4000 + 0x4000 d (lookat X to s, Y to t), clamped
                float d[2];
                for (int k2 = 0; k2 < 2; k2++) {
                    float x = nx * la[k2][0] + ny * la[k2][1] + nz * la[k2][2];
                    // Linear: the microcode's cubic (constants 0x44D3, 0x6CB3), not an arc cosine
                    float u = G.geom & GM_TGENL ? 16384.0f + 8809.4f * x + 7574.2f * x * x * x : 16384.0f + 16384.0f * x;
                    d[k2] = u < 0 ? 0 : u > 32767.0f ? 32767.0f : u;
                }
                s = d[0];
                t = d[1];
            }
        } else {
            v->r = c0; v->g = c1; v->b = c2;
        }
        v->s = s * (float)G.texScaleS / 65536.0f;
        v->t = t * (float)G.texScaleT / 65536.0f;
        // Screen position (viewport in quarter pixels)
        float iw = v->w != 0 ? 1.0f / v->w : 1.0f;
        v->sx = (v->x * iw * G.vscale[0] + G.vtrans[0]) / 4.0f;
        v->sy = (-v->y * iw * G.vscale[1] + G.vtrans[1]) / 4.0f;
        v->sz = (v->z * iw * G.vscale[2] + G.vtrans[2]) / 1024.0f;  // 0..1 for a standard viewport
        if (G.geom & GM_FOG) {
            float f = (v->z * iw) * G.fogMul + G.fogOff;
            v->a = f < 0 ? 0 : f > 255 ? 255 : f;
        }
        v->clip = (v->x < -v->w) | (v->x > v->w) << 1 | (v->y < -v->w) << 2 | (v->y > v->w) << 3 | (v->w < 0.01f) << 4;
        if (g_hleTrace && g_hleStats.tasks == g_hleTraceTask && k < 2)
            fprintf(stderr, "    vtx %d: raw s %d t %d c %02x%02x%02x%02x -> s %.1f t %.1f rgba %.0f %.0f %.0f %.0f, geom %06x lights %d "
                            "lookat (%.2f %.2f %.2f) (%.2f %.2f %.2f) scale %04x %04x\n", v0 + k, (s16)rd16(a + 8), (s16)rd16(a + 10),
                    c0, c1, c2, c3, v->s, v->t, v->r, v->g, v->b, v->a, (unsigned)G.geom, G.numLights, G.lookat[0][0],
                    G.lookat[0][1], G.lookat[0][2], G.lookat[1][0], G.lookat[1][1], G.lookat[1][2], (unsigned)G.texScaleS,
                    (unsigned)G.texScaleT);
    }
}

// ---- Triangle setup: three screen vertices into an RDP triangle command --------------------------------------------
static inline s32 fx16(float v) {  // s15.16, saturated
    double d = (double)v * 65536.0;
    if (d > 2147483647.0) d = 2147483647.0;
    if (d < -2147483648.0) d = -2147483648.0;
    return (s32)d;
}

typedef struct { float x, y, z, iw, s, t, r, g, b, a; } SV;

static void emit_triangle(const SV* p0, const SV* p1, const SV* p2) {
    const SV* v[3] = {p0, p1, p2};
    // Sort by y: h (top), m, l (bottom)
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2 - i; j++)
            if (v[j]->y > v[j + 1]->y) { const SV* t = v[j]; v[j] = v[j + 1]; v[j + 1] = t; }
    const SV *h = v[0], *m = v[1], *l = v[2];
    float yh = floorf(h->y * 4) / 4, ym = floorf(m->y * 4) / 4, yl = floorf(l->y * 4) / 4;
    if (yl - yh < 0.25f) return;
    float dxhdy = (l->y - h->y) > 0 ? (l->x - h->x) / (l->y - h->y) : 0;
    float dxmdy = (m->y - h->y) > 0 ? (m->x - h->x) / (m->y - h->y) : 0;
    float dxldy = (l->y - m->y) > 0 ? (l->x - m->x) / (l->y - m->y) : 0;
    // Left-major (lft = 1: spans run from the major edge rightward) when the middle vertex is right of the major edge
    float cross = (m->x - h->x) * (l->y - h->y) - (m->y - h->y) * (l->x - h->x);
    int lft = cross > 0 ? 1 : 0;
    float ytop = floorf(yh);                       // XH / XM at the top of YH's scanline
    float xhs = h->x + dxhdy * (ytop - h->y), xms = h->x + dxmdy * (ytop - h->y);
    float xls = m->x + dxldy * (ym - m->y);        // XL at YM
    bool shade = (G.geom & GM_SHADE) != 0, tex = G.texOn != 0, zb = (G.geom & GM_ZBUF) != 0;
    // Attribute planes: d/dx, d/dy from the three vertices; DaDe along the major edge; the value at (xhs, ytop)
    float det = (m->x - h->x) * (l->y - h->y) - (l->x - h->x) * (m->y - h->y);
    if (fabsf(det) < 1e-6f) return;
    float vals[3][8], dx[8], dy[8], start[8], de[8];
    // Perspective: S, T and W (= 1/w) vary linearly on screen; W normalized per triangle to fit s15.16
    float maxw = h->iw > m->iw ? h->iw : m->iw;
    if (l->iw > maxw) maxw = l->iw;
    float wn = maxw > 0 ? 32767.0f / maxw : 1.0f;
    const SV* vv[3] = {h, m, l};
    bool persp = (G.omH >> 19) & 1;  // the RDP divides S and T by W only with perspective correction on
    for (int k = 0; k < 3; k++) {
        float W = vv[k]->iw * wn, sw = persp ? W / 32768.0f : 1.0f;
        vals[k][0] = vv[k]->r; vals[k][1] = vv[k]->g; vals[k][2] = vv[k]->b; vals[k][3] = vv[k]->a;
        vals[k][4] = vv[k]->s * sw;
        vals[k][5] = vv[k]->t * sw;
        vals[k][6] = W;
        vals[k][7] = vv[k]->z * 32767.0f;
    }
    for (int a = 0; a < 8; a++) {
        float d1 = vals[1][a] - vals[0][a], d2 = vals[2][a] - vals[0][a];
        dx[a] = (d1 * (l->y - h->y) - d2 * (m->y - h->y)) / det;
        dy[a] = (d2 * (m->x - h->x) - d1 * (l->x - h->x)) / det;
        de[a] = dy[a] + dx[a] * dxhdy;
        start[a] = vals[0][a] + dx[a] * (xhs - h->x) + dy[a] * (ytop - h->y);
    }
    u32 id = 0x08 | (shade ? 4 : 0) | (tex ? 2 : 0) | (zb ? 1 : 0);
    u64 w[22];
    int n = 0;
    w[n++] = ((u64)id << 56) | ((u64)lft << 55) | ((u64)(G.texTile & 7) << 48) | ((u64)((s32)(yl * 4) & 0x3FFF) << 32) |
             ((u64)((s32)(ym * 4) & 0x3FFF) << 16) | (u64)((s32)(yh * 4) & 0x3FFF);
    w[n++] = ((u64)(u32)fx16(xls) << 32) | (u32)fx16(dxldy);
    w[n++] = ((u64)(u32)fx16(xhs) << 32) | (u32)fx16(dxhdy);
    w[n++] = ((u64)(u32)fx16(xms) << 32) | (u32)fx16(dxmdy);
    if (shade) {
        s32 c[4], cdx[4], cde[4], cdy[4];
        for (int k = 0; k < 4; k++) { c[k] = fx16(start[k]); cdx[k] = fx16(dx[k]); cde[k] = fx16(de[k]); cdy[k] = fx16(dy[k]); }
#define HI4(a) (((u64)((u32)(a)[0] >> 16) << 48) | ((u64)((u32)(a)[1] >> 16) << 32) | ((u64)((u32)(a)[2] >> 16) << 16) | ((u32)(a)[3] >> 16))
#define LO4(a) (((u64)((u32)(a)[0] & 0xFFFF) << 48) | ((u64)((u32)(a)[1] & 0xFFFF) << 32) | ((u64)((u32)(a)[2] & 0xFFFF) << 16) | ((u32)(a)[3] & 0xFFFF))
        w[n++] = HI4(c); w[n++] = HI4(cdx); w[n++] = LO4(c); w[n++] = LO4(cdx);
        w[n++] = HI4(cde); w[n++] = HI4(cdy); w[n++] = LO4(cde); w[n++] = LO4(cdy);
    }
    if (tex) {
        s32 c[4] = {fx16(start[4]), fx16(start[5]), fx16(start[6]), 0};
        s32 cdx[4] = {fx16(dx[4]), fx16(dx[5]), fx16(dx[6]), 0};
        s32 cde[4] = {fx16(de[4]), fx16(de[5]), fx16(de[6]), 0};
        s32 cdy[4] = {fx16(dy[4]), fx16(dy[5]), fx16(dy[6]), 0};
        w[n++] = HI4(c); w[n++] = HI4(cdx); w[n++] = LO4(c); w[n++] = LO4(cdx);
        w[n++] = HI4(cde); w[n++] = HI4(cdy); w[n++] = LO4(cde); w[n++] = LO4(cdy);
    }
    if (zb) {
        w[n++] = ((u64)(u32)fx16(start[7]) << 32) | (u32)fx16(dx[7]);
        w[n++] = ((u64)(u32)fx16(de[7]) << 32) | (u32)fx16(dy[7]);
    }
    rdp_out(w, n);
}

static SV to_sv(const V* v) {
    SV o;
    float iw = v->w != 0 ? 1.0f / v->w : 1.0f;
    // Screen x and y in the RSP's s13.2 (quarter pixels): unsnapped, nearly flat edges get huge slopes
    o.x = floorf(v->sx * 4) / 4; o.y = floorf(v->sy * 4) / 4; o.z = v->sz; o.iw = iw;
    o.s = v->s; o.t = v->t; o.r = v->r; o.g = v->g; o.b = v->b; o.a = v->a;
    if (o.z < 0) o.z = 0;
    if (o.z > 1) o.z = 1;
    return o;
}

// Clip-space vertex for near-plane clipping, then projected.
static V lerp_v(const V* a, const V* b, float t) {
    V o = *a;
    o.x = a->x + (b->x - a->x) * t; o.y = a->y + (b->y - a->y) * t;
    o.z = a->z + (b->z - a->z) * t; o.w = a->w + (b->w - a->w) * t;
    o.s = a->s + (b->s - a->s) * t; o.t = a->t + (b->t - a->t) * t;
    o.r = a->r + (b->r - a->r) * t; o.g = a->g + (b->g - a->g) * t;
    o.b = a->b + (b->b - a->b) * t; o.a = a->a + (b->a - a->a) * t;
    float iw = 1.0f / o.w;
    o.sx = (o.x * iw * G.vscale[0] + G.vtrans[0]) / 4.0f;
    o.sy = (-o.y * iw * G.vscale[1] + G.vtrans[1]) / 4.0f;
    o.sz = (o.z * iw * G.vscale[2] + G.vtrans[2]) / 1024.0f;
    return o;
}

static float clip_dist(const V* v, int plane) {  // >= 0: inside
    switch (plane) {
    case 0: return v->z + v->w;
    case 1: return v->w - 0.001f;
    case 2: return 2 * v->w + v->x;
    case 3: return 2 * v->w - v->x;
    case 4: return 2 * v->w + v->y;
    default: return 2 * v->w - v->y;
    }
}

#define TRI_TRACE(why)                                                                                                   \
    do {                                                                                                                 \
        if (g_hleTrace && g_hleStats.tasks == g_hleTraceTask)                                                            \
            fprintf(stderr, "    tri %d %d %d %s: (%.1f %.1f %.3f w%.2f) (%.1f %.1f %.3f w%.2f) (%.1f %.1f %.3f w%.2f) geom %06x\n", \
                    i0, i1, i2, why, a->sx, a->sy, a->sz, a->w, b->sx, b->sy, b->sz, b->w, c->sx, c->sy, c->sz, c->w,     \
                    (unsigned)G.geom);                                                                                   \
    } while (0)
static void tri(int i0, int i1, int i2) {
    if (i0 >= 64 || i1 >= 64 || i2 >= 64) return;
    V* a = &G.vtx[i0];
    V* b = &G.vtx[i1];
    V* c = &G.vtx[i2];
    if (a->clip & b->clip & c->clip & 0xF) { TRI_TRACE("outside"); return; }  // all outside one side plane
    // Flat shading: the first vertex's colour for all three
    V fa = *a, fb = *b, fc = *c;
    if (!(G.geom & GM_SMOOTH)) {
        fb.r = fc.r = fa.r; fb.g = fc.g = fa.g; fb.b = fc.b = fa.b; fb.a = fc.a = fa.a;
        // F3D's flat shading takes the first vertex too
    }
    // Clipping in clip space, as the RSP does: the near plane (z >= -w, and w > 0) and the four sides with a guard
    // band of twice the screen. Clipping only at the near plane left vertices thousands of pixels off screen, and the
    // coordinate limit then bent the triangles (Ocarina of Time's field drew no ground)
    V buf[2][12];
    int np = 3;
    buf[0][0] = fa; buf[0][1] = fb; buf[0][2] = fc;
    int cur = 0;
    for (int plane = 0; plane < 6 && np >= 3; plane++) {
        V* src = buf[cur];
        V* dst = buf[cur ^ 1];
        int n = 0;
        for (int k = 0; k < np; k++) {
            const V* p = &src[k];
            const V* q = &src[(k + 1) % np];
            float dp = clip_dist(p, plane), dq = clip_dist(q, plane);
            if (dp >= 0) dst[n++] = *p;
            if ((dp >= 0) != (dq >= 0) && n < 12) dst[n++] = lerp_v(p, q, dp / (dp - dq));
        }
        np = n;
        cur ^= 1;
    }
    V* poly = buf[cur];
    if (np < 3) { TRI_TRACE("behind"); return; }
    // Culling on the screen winding
    SV s[12];
    for (int k = 0; k < np; k++) s[k] = to_sv(&poly[k]);
    float area = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (s[1].y - s[0].y);
    // Front faces are counter-clockwise with y up: a negative area here, where screen y points down
    if ((G.geom & GM_CULLB) && area > 0) { TRI_TRACE("culled back"); return; }
    if ((G.geom & GM_CULLF) && area < 0) { TRI_TRACE("culled front"); return; }
    for (int k = 0; k < np; k++) {  // keep coordinates inside the RDP's range
        if (s[k].x < -1024) s[k].x = -1024;
        if (s[k].x > 2047) s[k].x = 2047;
        if (s[k].y < -1024) s[k].y = -1024;
        if (s[k].y > 2047) s[k].y = 2047;
    }
    TRI_TRACE("drawn");
    g_hleStats.triangles += (u64)(np - 2);  // counted here for both renderers (the GPU path emits no RDP triangles)
    G.taskTris += (u64)(np - 2);
    if (g_hleVtxSink) {
        HleVtx o[12];
        for (int k = 0; k < np; k++) {
            const V* p = &poly[k];
            o[k] = (HleVtx){p->sx, p->sy, s[k].z, p->w, p->s, p->t, s[k].r, s[k].g, s[k].b, s[k].a};
        }
        g_hleVtxSink(o, np, (G.geom & GM_SHADE ? 4u : 0) | (G.texOn ? 2u : 0) | (G.geom & GM_ZBUF ? 1u : 0) | (G.texTile & 7) << 8);
        return;
    }
    for (int k = 1; k + 1 < np; k++) emit_triangle(&s[0], &s[k], &s[k + 1]);
}

// ---- RDP passthrough ----------------------------------------------------------------------------------------------
static void rdp1(u32 w0, u32 w1) {
    u64 c = ((u64)w0 << 32) | w1;
    u32 op = (w0 >> 24) & 0x3F;
    if (op == 0x3D || op == 0x3E || op == 0x3F) c = (c & ~0xFFFFFFFFull) | seg_addr(w1);  // image addresses
    rdp_out(&c, 1);
    if (op == 0x29) G.fullSync = true;
    // A whole SET_OTHER_MODES updates the microcode's copy too, which later partial SETOTHERMODE_H / _L merge into
    // (without it Ocarina of Time's field ran in fill mode and drew no ground)
    if (op == 0x2F) { G.omH = w0 & 0xFFFFFF; G.omL = w1; }
}

static void set_othermode(bool high, u32 shift, u32 len, u32 data) {
    u32 mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
    u64* om = high ? &G.omH : &G.omL;
    *om = (*om & ~(u64)mask) | (data & mask);
    u64 c = (0x2Full << 56) | ((G.omH & 0xFFFFFF) << 32) | (G.omL & 0xFFFFFFFF);
    rdp_out(&c, 1);
}

// ---- The display list ---------------------------------------------------------------------------------------------
static void lights_from(u32 a, int index) {
    if (index < 0 || index > 7) return;
    G.lightCol[index][0] = rd8(a); G.lightCol[index][1] = rd8(a + 1); G.lightCol[index][2] = rd8(a + 2);
    float x = (s8)rd8(a + 8), y = (s8)rd8(a + 9), z = (s8)rd8(a + 10);
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 0) { x /= len; y /= len; z /= len; }
    G.lightDir[index][0] = x; G.lightDir[index][1] = y; G.lightDir[index][2] = z;
}

static void viewport(u32 a) {
    for (int k = 0; k < 4; k++) { G.vscale[k] = (s16)rd16(a + (u32)k * 2); G.vtrans[k] = (s16)rd16(a + 8 + (u32)k * 2); }
}

static void moveword(u32 index, u32 offset, u32 v) {
    switch (index) {
    case 2:  // NUMLIGHT
        if (G.uc == UC_F3DEX2) G.numLights = (int)(v / 24);
        else G.numLights = (int)(((v - 0x80000000u) >> 5) - 1);
        if (G.numLights < 0) G.numLights = 0;
        if (G.numLights > 7) G.numLights = 7;
        break;
    case 6: G.seg[(offset >> 2) & 15] = v & 0xFFFFFF; break;  // SEGMENT
    case 8: G.fogMul = (s16)(v >> 16); G.fogOff = (s16)v; break;  // FOG
    case 10: {  // LIGHTCOL: offset selects the light (F3D: 32 bytes each; F3DEX2: 24)
        int l = G.uc == UC_F3DEX2 ? (int)(offset / 24) : (int)((offset >> 5) & 7);
        if (l >= 0 && l < 8 && !(offset & 4)) { G.lightCol[l][0] = (u8)(v >> 24); G.lightCol[l][1] = (u8)(v >> 16); G.lightCol[l][2] = (u8)(v >> 8); }
        break;
    }
    default: break;
    }
}

static bool run_dl(u32 pc) {
    u32 stack[32];
    int sp = 0;
    for (u32 guard = 0; guard < 2000000; guard++) {
        u32 w0 = rd32(pc), w1 = rd32(pc + 4);
        pc += 8;
        G.cmds++;
        u32 op = w0 >> 24;
        if (g_hleTrace && g_hleStats.tasks == g_hleTraceTask)
            fprintf(stderr, "  dl %06x: %08x %08x%s\n", (unsigned)(pc - 8), (unsigned)w0, (unsigned)w1, sp ? "" : " (top)");
        // RDP commands (0xE6 and up; F3DEX2's RDPHALF_2 0xF1 is its own) go to the RDP as they are
        if (op >= 0xE6 && !(G.uc == UC_F3DEX2 && op == 0xF1)) {
            rdp1(w0, w1);
            continue;
        }
        if (op == 0xE4 || op == 0xE5) {  // texture rectangle: the next two commands carry its second word
            u32 a = rd32(pc + 4), b = rd32(pc + 12);
            pc += 16;
            u64 c[2] = {((u64)w0 << 32) | w1, ((u64)a << 32) | b};
            rdp_out(c, 2);
            continue;
        }
        if (G.uc == UC_F3DEX2) {
            switch (op) {
            case 0x00: break;
            case 0x01: {  // VTX
                int n = (int)((w0 >> 12) & 0xFF), v0 = (int)((w0 >> 1) & 0x7F) - n;
                if (v0 >= 0) load_vertices(seg_addr(w1), v0, n);
                break;
            }
            case 0x02: {  // MODIFYVTX
                int v = (int)((w0 & 0xFFFF) >> 1);
                u32 where = (w0 >> 16) & 0xFF;
                if (v < 64) {
                    V* x = &G.vtx[v];
                    if (where == 0x10) { x->r = (float)(w1 >> 24); x->g = (float)((w1 >> 16) & 255); x->b = (float)((w1 >> 8) & 255); x->a = (float)(w1 & 255); }
                    else if (where == 0x14) { x->s = (float)(s16)(w1 >> 16); x->t = (float)(s16)w1; }
                    else if (where == 0x18) { x->sx = (float)(s16)(w1 >> 16) / 4; x->sy = (float)(s16)w1 / 4; }
                }
                break;
            }
            case 0x05: tri((int)((w0 >> 16) & 0xFF) / 2, (int)((w0 >> 8) & 0xFF) / 2, (int)(w0 & 0xFF) / 2); break;
            case 0x06: case 0x07:
                tri((int)((w0 >> 16) & 0xFF) / 2, (int)((w0 >> 8) & 0xFF) / 2, (int)(w0 & 0xFF) / 2);
                tri((int)((w1 >> 16) & 0xFF) / 2, (int)((w1 >> 8) & 0xFF) / 2, (int)(w1 & 0xFF) / 2);
                break;
            case 0x04: {  // BRANCH_Z: to RDPHALF_1's address when the vertex is nearer than the value
                int v = (int)(w0 & 0xFFF) / 2;  // the vertex x5 in bits 12-23, x2 in 0-11; w1 = G_DEPTOZS: 0x3FF x 16.16
                if (v < 64 && G.vtx[v].sz * 0x3FF * 65536.0f <= (float)(s32)w1) pc = seg_addr(G.half1);
                break;
            }
            case 0xD7:  // TEXTURE
                G.texOn = (w0 >> 1) & 0x7F; G.texTile = (w0 >> 8) & 7;
                G.texScaleS = w1 >> 16; G.texScaleT = w1 & 0xFFFF;
                break;
            case 0xD8: {  // POPMTX
                int n = (int)(w1 / 64);
                G.mvDepth = G.mvDepth > n ? G.mvDepth - n : 0;
                G.mvpDirty = true;
                break;
            }
            case 0xD9: G.geom = (G.geom & (w0 & 0xFFFFFF)) | w1; break;  // GEOMETRYMODE
            case 0xDA: {  // MTX: params ^ PUSH in the low byte
                u32 p = (w0 & 0xFF) ^ 1;
                do_mtx(seg_addr(w1), p & 4, p & 2, p & 1);
                break;
            }
            case 0xDB: moveword((w0 >> 16) & 0xFF, w0 & 0xFFFF, w1); break;
            case 0xDC: {  // MOVEMEM
                u32 index = w0 & 0xFF, off = ((w0 >> 8) & 0xFF) * 8, a = seg_addr(w1);
                if (index == 8) viewport(a);
                else if (index == 10) {  // LIGHT: lookat at offsets 0 / 24, lights from 48
                    int l = (int)(off / 24) - 2;
                    if (l < 0) {
                        int k = (int)(off / 24);
                        float x = (s8)rd8(a + 8), y = (s8)rd8(a + 9), z = (s8)rd8(a + 10), len = sqrtf(x * x + y * y + z * z);
                        if (len > 0 && k < 2) { G.lookat[k][0] = x / len; G.lookat[k][1] = y / len; G.lookat[k][2] = z / len; }
                    } else {
                        lights_from(a, l);
                    }
                }
                break;
            }
            case 0xDE:  // DL
                if (!((w0 >> 16) & 1) && sp < 32) stack[sp++] = pc;
                pc = seg_addr(w1);
                break;
            case 0xDF:  // ENDDL
                if (!sp) return true;
                pc = stack[--sp];
                break;
            case 0xE1: G.half1 = w1; break;
            case 0xF1: G.half2 = w1; break;
            case 0xE2: case 0xE3: {  // SETOTHERMODE_L / _H: (32 - shift - len) << 8 | (len - 1)
                u32 len = (w0 & 0xFF) + 1, shift = 32 - ((w0 >> 8) & 0xFF) - len;
                set_othermode(op == 0xE3, shift, len, w1);
                break;
            }
            default: break;  // CULLDL, LINE3D, SPECIAL, DMA_IO, LOAD_UCODE: not handled (no-op)
            }
            continue;
        }
        // F3D / F3DEX
        switch (op) {
        case 0x00: break;
        case 0x01: do_mtx(seg_addr(w1), (w0 >> 16) & 1, (w0 >> 16) & 2, (w0 >> 16) & 4); break;
        case 0x03: {  // MOVEMEM
            u32 index = (w0 >> 16) & 0xFF, a = seg_addr(w1);
            if (index == 0x80) viewport(a);
            else if (index == 0x82 || index == 0x84) {
                int k = index == 0x84 ? 0 : 1;
                float x = (s8)rd8(a + 8), y = (s8)rd8(a + 9), z = (s8)rd8(a + 10), len = sqrtf(x * x + y * y + z * z);
                if (len > 0) { G.lookat[k][0] = x / len; G.lookat[k][1] = y / len; G.lookat[k][2] = z / len; }
            } else if (index >= 0x86 && index <= 0x94) {
                lights_from(a, (int)(index - 0x86) / 2);
            }
            break;
        }
        case 0x04:  // VTX
            if (G.uc == UC_F3D && G.triDiv == 5)  // Wave Race: count << 9 | length - 1, first index x5
                load_vertices(seg_addr(w1), (int)((w0 >> 16) & 0xFF) / 5, (int)((w0 >> 9) & 0x7F));
            else if (G.uc == UC_F3D) load_vertices(seg_addr(w1), (int)((w0 >> 16) & 0xF), (int)((w0 >> 20) & 0xF) + 1);
            else load_vertices(seg_addr(w1), (int)((w0 >> 16) & 0xFF) / 2, (int)((w0 >> 10) & 0x3F));
            break;
        case 0x06:  // DL
            if (!((w0 >> 16) & 1) && sp < 32) stack[sp++] = pc;
            pc = seg_addr(w1);
            break;
        case 0xBF: {  // TRI1: indices x10 (F3D) or x2 (F3DEX)
            int d = G.uc == UC_F3D ? G.triDiv : 2;
            tri((int)((w1 >> 16) & 0xFF) / d, (int)((w1 >> 8) & 0xFF) / d, (int)(w1 & 0xFF) / d);
            break;
        }
        case 0xB1:  // TRI2 (F3DEX); in Fast3D, Rare's TRI4 (GoldenEye): four triangles of 4-bit indices (GLideN64 F3D_Tri4)
            if (G.uc == UC_F3DEX) {
                tri((int)((w0 >> 16) & 0xFF) / 2, (int)((w0 >> 8) & 0xFF) / 2, (int)(w0 & 0xFF) / 2);
                tri((int)((w1 >> 16) & 0xFF) / 2, (int)((w1 >> 8) & 0xFF) / 2, (int)(w1 & 0xFF) / 2);
            } else {
                for (int k = 0; k < 4; k++) {
                    int a = (int)(w0 >> (4 * k)) & 15, b = (int)(w1 >> (8 * k)) & 15, c = (int)(w1 >> (8 * k + 4)) & 15;
                    if (a != b || b != c) tri(a, b, c);
                }
            }
            break;
        case 0xB5:  // QUAD (F3DEX, x2; Wave Race's Fast3D, x5; LINE3D otherwise, not drawn): as (v0, v1, v2), (v0, v2, v3)
            if (G.uc == UC_F3DEX || G.triDiv == 5) {
                int q = G.uc == UC_F3DEX ? 2 : 5;
                int a = (int)(w1 >> 24) / q, b = (int)((w1 >> 16) & 0xFF) / q, c = (int)((w1 >> 8) & 0xFF) / q, d = (int)(w1 & 0xFF) / q;
                tri(a, b, c);
                tri(a, c, d);
            }
            break;
        case 0xB0:  // BRANCH_Z (F3DEX)
            if (G.uc == UC_F3DEX) {
                int v = (int)(w0 & 0xFFF) / 2;
                if (v < 64 && G.vtx[v].sz * 0x3FF * 65536.0f <= (float)(s32)w1) pc = seg_addr(G.half1);
            }
            break;
        case 0xBD: if (G.mvDepth > 0) G.mvDepth--; G.mvpDirty = true; break;  // POPMTX
        case 0xBC: moveword(w0 & 0xFF, (w0 >> 8) & 0xFFFF, w1); break;          // MOVEWORD
        case 0xBB:  // TEXTURE
            G.texOn = w0 & 0xFF; G.texTile = (w0 >> 8) & 7;
            G.texScaleS = w1 >> 16; G.texScaleT = w1 & 0xFFFF;
            break;
        case 0xBA: case 0xB9:  // SETOTHERMODE_H / _L: shift << 8 | len
            set_othermode(op == 0xBA, (w0 >> 8) & 0xFF, w0 & 0xFF, w1);
            break;
        case 0xB8:  // ENDDL
            if (!sp) return true;
            pc = stack[--sp];
            break;
        case 0xB7: G.geom |= w1; break;
        case 0xB6: G.geom &= ~w1; break;
        case 0xB4: G.half1 = w1; break;
        case 0xB3: G.half2 = w1; break;
        default: break;  // CULLDL, LINE3D, RDPHALF_CONT, MODIFYVTX (F3DEX), LOAD_UCODE: not handled
        }
    }
    return false;  // runaway display list
}

// ---- Entry --------------------------------------------------------------------------------------------------------
// What the microcode keeps in DMEM is reloaded from its data segment (ucode_data) at every task, so state a display
// list did not set is the data segment's default, not the previous task's. F3D and F3DEX 1.x: a table of DMEM
// addresses (viewport, lookat Y, lookat X, lights 0-7, each light 0x20 apart) and the other-mode command (0xEF) are
// found by their shape in the data segment; the lookat vectors and lights are read from the addresses it gives.
static void lookat_from(int k, u32 a) {
    float x = (s8)rd8(a + 8), y = (s8)rd8(a + 9), z = (s8)rd8(a + 10), len = sqrtf(x * x + y * y + z * z);
    G.lookat[k][0] = len > 0 ? x / len : 0; G.lookat[k][1] = len > 0 ? y / len : 0; G.lookat[k][2] = len > 0 ? z / len : 0;
}

static void task_defaults(u32 data, u32 size) {
    G.mvDepth = 0;
    G.mvpDirty = true;
    memset(G.seg, 0, sizeof G.seg);
    G.geom = 0;
    G.numLights = 0;
    u32 lim = size < 0x800 ? size : 0x800;
    for (u32 i = 0; i + 8 <= lim && i < 0x400; i += 4)
        if (rd8(data + i) == 0xEF) { G.omH = rd32(data + i) & 0xFFFFFF; G.omL = rd32(data + i + 4); break; }
    if (G.uc == UC_F3DEX2) return;  // (its lights and lookat are kept elsewhere: not found yet)
    for (u32 t = 0x100; t + 22 <= lim; t += 2) {
        u16 e[11];
        for (int k = 0; k < 11; k++) e[k] = rd16(data + t + (u32)k * 2);
        bool ok = e[1] + 0x20 == e[2];
        for (int k = 3; k < 10 && ok; k++) ok = e[k] + 0x20 == e[k + 1] && e[k + 1] < lim;
        if (!ok || e[2] + 0x20 != e[3]) continue;
        lookat_from(1, data + e[1]);  // G_MV_LOOKATY (0x82)
        lookat_from(0, data + e[2]);  // G_MV_LOOKATX (0x84)
        for (int l = 0; l < 8; l++) lights_from(data + e[3 + l], l);
        break;
    }
}

static int identify(u32 data, u32 size) {
    char s[96];
    for (u32 i = 0; i + 4 < size && i < 0x1000; i++) {
        if (rd8(data + i) != 'R' || rd8(data + i + 1) != 'S' || rd8(data + i + 2) != 'P') continue;
        u32 n = 0;
        for (; n < sizeof s - 1; n++) {
            u8 c = rd8(data + i + n);
            if (c < 32 || c > 126) break;
            s[n] = (char)c;
        }
        s[n] = 0;
        if (strstr(s, "RSP SW Version: 2.0")) return UC_F3D;
        if (strstr(s, "F3DZEX") || strstr(s, "F3DEX2")) return UC_F3DEX2;
        if (strstr(s, "F3DEX") || strstr(s, "F3DLX") || strstr(s, "F3DLP")) {
            const char* v = strstr(s, "fifo");
            const char* d = v ? v + 5 : s + 20;
            while (*d == ' ') d++;
            return (strstr(s, " 2.0") || (v && *d == '2')) ? UC_F3DEX2 : UC_F3DEX;
        }
        return UC_NONE;
    }
    return UC_NONE;
}

// Microcodes that share a version string with a standard one but differ, by a hash of their first 4 KB of text
// (FNV-1a), with what differs. Found with N64_HLE_TRACE, which prints each new microcode's hash.
static const struct { u32 hash; int triDiv; const char* name; } g_variants[] = {
    {0x4c283670, 5, "(Wave Race 64 USA: triangle and quad indices x5, GLideN64 F3DWRUS)"},
    {0, 0, NULL},
};

static u32 text_hash(u32 text) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < 0x1000; i++) h = (h ^ rd8(text + i)) * 16777619u;
    return h;
}

// A task in two halves: hle_gfx_prepare (identification and the per-task DMEM defaults, from the OSTask in DMEM:
// on core 0 at the task's start) and hle_gfx_run (the display list: on core 2 when the host has a worker, src/sys/sp.c
// g_hleAsync). Between them nothing else touches G.
bool hle_gfx_prepare(void) {
    u32 text = m_r32(g_mem.spmem, 0xFD0) & 0xFFFFFF;
    u32 data = m_r32(g_mem.spmem, 0xFD8) & 0xFFFFFF, size = m_r32(g_mem.spmem, 0xFDC);
    static u32 lastText = 0xFFFFFFFF;
    static int lastUc, lastDiv;
    if (text != lastText) {
        lastText = text;
        lastUc = identify(data, size);
        lastDiv = 10;
        u32 h = text_hash(text);
        const char* name = "";
        for (u32 k = 0; g_variants[k].name; k++)
            if (g_variants[k].hash == h) { lastDiv = g_variants[k].triDiv; name = g_variants[k].name; }
        if (g_hleTrace) fprintf(stderr, "hle: microcode at %06x, text hash %08x: dialect %d %s\n", (unsigned)text, (unsigned)h, lastUc, name);
        if (g_hleTrace) {  // its DMEM image (ucode_data), for finding defaults the display lists rely on
            char p[64];
            snprintf(p, sizeof p, "/tmp/ucdata_%06x.bin", (unsigned)data);
            FILE* f = fopen(p, "wb");
            for (u32 i = 0; f && i < size && i < 0x1000; i++) fputc(rd8(data + i), f);
            if (f) fclose(f);
            fprintf(stderr, "hle: ucode data %06x, %u bytes -> %s\n", (unsigned)data, (unsigned)size, p);
        }
    }
    if (lastUc == UC_NONE) { g_hleStats.unknown++; return false; }
    if (G.uc != lastUc) set_dialect(lastUc);
    G.triDiv = lastDiv;
    task_defaults(data, size);
    G.fullSync = false;
    G.cmds = 0;
    G.taskTris = 0;
    G.dl = m_r32(g_mem.spmem, 0xFF0) & 0xFFFFFF;  // OSTask data_ptr
    return true;
}

void hle_gfx_run(void) {
    run_dl(G.dl);
    g_hleStats.tasks++;
    g_hleStats.commands += G.cmds;
}

bool hle_gfx_task(void) {
    if (!hle_gfx_prepare()) return false;
    hle_gfx_run();
    if (G.fullSync) rdp_full_sync();
    return true;
}

void hle_reset(void) {
    memset(&G, 0, sizeof G);
    for (int i = 0; i < 18; i++) for (int k = 0; k < 4; k++) G.mv[i].m[k][k] = 1;
    for (int k = 0; k < 4; k++) G.proj.m[k][k] = 1;
    G.mvpDirty = true;
    memset(&g_hleStats, 0, sizeof g_hleStats);
    const char* t = getenv("N64_HLE_TRACE");
    g_hleTrace = t != NULL;
    g_hleTraceTask = t ? strtoull(t, NULL, 10) : 0;
}

u64 hle_last_commands(void) { return G.cmds; }
u64 hle_last_triangles(void) { return G.taskTris; }
bool hle_last_full_sync(void) { return G.fullSync; }
