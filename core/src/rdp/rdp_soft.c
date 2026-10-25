// Software RDP: draws the RDP's commands into RDRAM (the colour and Z images), so the VI shows what a game rendered.
// A readable reference renderer, not a bit-exact one: one sample per pixel (no coverage, antialiasing or dithering),
// point or bilinear texture sampling, the colour combiner (both cycles), the blender's common forms, alpha compare
// and a linear 16-bit Z. Formats and command layouts follow the RDP documentation on n64brew ("Reality Display
// Processor / Commands"); PeterLemon's RDP ROMs (hardware captures) measure how close it gets
// (core/tools/peterlemon.py, docs/TEST_ROMS.md).
#include <string.h>
#include "../sys/sys.h"

// ---- State --------------------------------------------------------------------------------------------------------
typedef struct {
    u32 fmt, size, line, tmem, palette;
    u32 cms, cmt, masks, maskt, shifts, shiftt;  // cm: bit 0 mirror, bit 1 clamp
    u32 sl, tl, sh, th;                          // 10.2
} Tile;

static struct {
    u32 ciAddr, ciFmt, ciSize, ciWidth;          // colour image
    u32 zAddr;
    u32 tiAddr, tiFmt, tiSize, tiWidth;          // texture image
    u32 sxh, syh, sxl, syl;                      // scissor, 10.2
    u64 om;                                      // other modes
    u64 combine;
    u32 fill, prim, env, blend, fog;             // RGBA8888 colours (fill: raw)
    u32 primLod, primMinLod, primZ;
    Tile tiles[8];
    u8 tmem[4096];
} R;

void rdp_soft_reset(void);
void rdp_soft_reset(void) { memset(&R, 0, sizeof R); }

#define OM(bit, n) ((u32)(R.om >> (bit)) & ((1u << (n)) - 1))
enum { CYC1 = 0, CYC2 = 1, COPY = 2, FILL = 3 };
enum { F_RGBA = 0, F_YUV = 1, F_CI = 2, F_IA = 3, F_I = 4 };

static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline u32 rgba(int r, int g, int b, int a) { return ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | (u32)a; }

// ---- Colour image access ------------------------------------------------------------------------------------------
static u32 fb_read(u32 x, u32 y) {
    u32 i = y * R.ciWidth + x;
    if (R.ciSize == 2) {  // 16-bit RGBA5551
        u16 p = m_r16(g_mem.rdram, (R.ciAddr + i * 2) & (g_mem.rdramSize - 1));
        int r = (p >> 11) & 31, g = (p >> 6) & 31, b = (p >> 1) & 31;
        return rgba(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, (p & 1) ? 255 : 0);
    }
    if (R.ciSize == 3) return m_r32(g_mem.rdram, (R.ciAddr + i * 4) & (g_mem.rdramSize - 1));
    u8 v = m_r8(g_mem.rdram, (R.ciAddr + i) & (g_mem.rdramSize - 1));
    return rgba(v, v, v, v);
}

static void fb_write(u32 x, u32 y, u32 c) {
    u32 i = y * R.ciWidth + x;
    if (R.ciSize == 2) {
        u16 p = (u16)(((c >> 27) & 31) << 11 | ((c >> 19) & 31) << 6 | ((c >> 11) & 31) << 1 | ((c & 0xFF) >= 0x80));
        m_w16(g_mem.rdram, (R.ciAddr + i * 2) & (g_mem.rdramSize - 1), p);
    } else if (R.ciSize == 3) {
        m_w32(g_mem.rdram, (R.ciAddr + i * 4) & (g_mem.rdramSize - 1), c);
    } else {
        m_w8(g_mem.rdram, (R.ciAddr + i) & (g_mem.rdramSize - 1), (u8)(c >> 24));
    }
}

static u16 z_read(u32 x, u32 y) { return m_r16(g_mem.rdram, (R.zAddr + (y * R.ciWidth + x) * 2) & (g_mem.rdramSize - 1)); }
static void z_write(u32 x, u32 y, u16 z) { m_w16(g_mem.rdram, (R.zAddr + (y * R.ciWidth + x) * 2) & (g_mem.rdramSize - 1), z); }

// ---- Textures -----------------------------------------------------------------------------------------------------
static inline u8 tm8(u32 a) { return R.tmem[a & 0xFFF]; }
static inline u16 tm16(u32 a) { return (u16)((tm8(a) << 8) | tm8(a + 1)); }

static u32 tlut(u32 index) {
    u16 e = tm16(0x800 + (index & 0xFF) * 8);
    if (OM(46, 1)) return rgba(e >> 8, e >> 8, e >> 8, e & 0xFF);  // IA16 palette
    int r = (e >> 11) & 31, g = (e >> 6) & 31, b = (e >> 1) & 31;
    return rgba(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, (e & 1) ? 255 : 0);
}

// The texel at integer (s, t) of a tile, already wrapped / clamped.
static u32 texel(const Tile* tl, int s, int t) {
    u32 base = tl->tmem * 8 + (u32)t * tl->line * 8;
    u32 swap = (t & 1) ? 4 : 0;  // odd rows have their 32-bit words swapped in TMEM
    switch (tl->size) {
    case 0: {  // 4-bit
        u8 b = tm8((base + (u32)s / 2) ^ swap);
        u32 v = (s & 1) ? (b & 15u) : (b >> 4);
        if (tl->fmt == F_CI || OM(47, 1)) return tlut(tl->palette * 16 + v);
        if (tl->fmt == F_IA) { u32 i = (v >> 1) * 255 / 7; return rgba(i, i, i, (v & 1) ? 255 : 0); }
        u32 i = v * 17;
        return rgba(i, i, i, i);
    }
    case 1: {  // 8-bit
        u8 v = tm8((base + (u32)s) ^ swap);
        if (tl->fmt == F_CI || OM(47, 1)) return tlut(v);
        if (tl->fmt == F_IA) { u32 i = (v >> 4) * 17u; return rgba(i, i, i, (v & 15) * 17); }
        return rgba(v, v, v, v);
    }
    case 2: {  // 16-bit
        u16 v = tm16((base + (u32)s * 2) ^ swap);
        if (tl->fmt == F_IA) return rgba(v >> 8, v >> 8, v >> 8, v & 0xFF);
        if (tl->fmt == F_CI || OM(47, 1)) return tlut(v >> 8);
        int r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
        return rgba(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, (v & 1) ? 255 : 0);
    }
    default: {  // 32-bit: red / green in the low half of TMEM, blue / alpha in the high half
        u32 a = (base + (u32)s * 2) ^ swap;
        u16 rg = tm16(a & 0x7FF), ba = tm16(0x800 | (a & 0x7FF));
        return rgba(rg >> 8, rg & 0xFF, ba >> 8, ba & 0xFF);
    }
    }
}

// One axis: shift, offset by the tile's start, clamp, mirror, mask. `c` is in 10.5 texel units; returns texels.
static int wrap(int c, u32 shift, u32 lo, u32 hi, u32 cm, u32 mask, int* frac) {
    if (shift && shift <= 10) c >>= shift;
    else if (shift > 10) c <<= (16 - shift);
    c -= (int)lo << 3;
    *frac = c & 31;
    int t = c >> 5;
    int maxv = ((int)hi - (int)lo) >> 2;
    if ((cm & 2) || !mask) {
        if (c < 0) { t = 0; *frac = 0; }
        else if (t > maxv) { t = maxv; *frac = 0; }
    }
    if (mask) {
        if ((cm & 1) && ((t >> mask) & 1)) t = ~t;
        t &= (1 << mask) - 1;
    }
    return t;
}

static u32 sample(int tile, s32 s105, s32 t105) {
    const Tile* tl = &R.tiles[tile & 7];
    int fs, ft;
    int s = wrap(s105, tl->shifts, tl->sl, tl->sh, tl->cms, tl->masks, &fs);
    int t = wrap(t105, tl->shiftt, tl->tl, tl->th, tl->cmt, tl->maskt, &ft);
    if (!OM(45, 1) || OM(53, 2) == COPY) return texel(tl, s, t);  // point sampling
    // Bilinear: the four neighbours (the second wrapped the same way)
    int fs2, ft2;
    int s1 = wrap(s105 + 32 * (tl->shifts && tl->shifts <= 10 ? (1 << tl->shifts) : 1), tl->shifts, tl->sl, tl->sh,
                  tl->cms, tl->masks, &fs2);
    int t1 = wrap(t105 + 32 * (tl->shiftt && tl->shiftt <= 10 ? (1 << tl->shiftt) : 1), tl->shiftt, tl->tl, tl->th,
                  tl->cmt, tl->maskt, &ft2);
    u32 c00 = texel(tl, s, t), c10 = texel(tl, s1, t), c01 = texel(tl, s, t1), c11 = texel(tl, s1, t1);
    u32 out = 0;
    for (int k = 24; k >= 0; k -= 8) {
        int a = (c00 >> k) & 255, b = (c10 >> k) & 255, c = (c01 >> k) & 255, d = (c11 >> k) & 255;
        int top = a + ((b - a) * fs >> 5), bot = c + ((d - c) * fs >> 5);
        out |= (u32)clamp8(top + ((bot - top) * ft >> 5)) << k;
    }
    return out;
}

// ---- Combiner and blender -----------------------------------------------------------------------------------------
typedef struct { int r, g, b, a; } Col;
static inline Col col(u32 c) { Col x = {(int)(c >> 24), (int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255)}; return x; }

static Col combine_cycle(int cyc, Col comb, Col t0, Col t1, Col shade) {
    u64 m = R.combine;
    u32 ar = cyc ? (u32)(m >> 37) & 15 : (u32)(m >> 52) & 15;
    u32 cr = cyc ? (u32)(m >> 32) & 31 : (u32)(m >> 47) & 31;
    u32 br = cyc ? (u32)(m >> 24) & 15 : (u32)(m >> 28) & 15;
    u32 dr = cyc ? (u32)(m >> 6) & 7 : (u32)(m >> 15) & 7;
    u32 aa = cyc ? (u32)(m >> 21) & 7 : (u32)(m >> 44) & 7;
    u32 ca = cyc ? (u32)(m >> 18) & 7 : (u32)(m >> 41) & 7;
    u32 ba = cyc ? (u32)(m >> 3) & 7 : (u32)(m >> 12) & 7;
    u32 da = cyc ? (u32)m & 7 : (u32)(m >> 9) & 7;
    Col prim = col(R.prim), env = col(R.env), one = {255, 255, 255, 255}, zero = {0, 0, 0, 0};
    Col src[8] = {comb, t0, t1, prim, shade, env, one, zero};
    Col A = ar < 7 ? src[ar] : (ar == 7 ? zero /* noise */ : zero);
    Col B = br < 6 ? src[br] : zero;
    Col D = dr < 7 ? src[dr] : zero;
    Col C;
    switch (cr) {
    case 0: case 1: case 2: case 3: case 4: case 5: C = src[cr]; break;
    case 7: C.r = C.g = C.b = comb.a; break;
    case 8: C.r = C.g = C.b = t0.a; break;
    case 9: C.r = C.g = C.b = t1.a; break;
    case 10: C.r = C.g = C.b = prim.a; break;
    case 11: C.r = C.g = C.b = shade.a; break;
    case 12: C.r = C.g = C.b = env.a; break;
    case 14: C.r = C.g = C.b = (int)R.primLod; break;
    default: C = zero; break;
    }
    Col asrc[8] = {comb, t0, t1, prim, shade, env, one, zero};
    int Aa = asrc[aa].a, Ba = asrc[ba].a, Da = asrc[da].a;
    int Ca = ca == 0 ? 0 : ca == 6 ? (int)R.primLod : ca == 7 ? 0 : asrc[ca].a;
    Col o;
    o.r = clamp8(((A.r - B.r) * C.r + 0x80) / 256 + D.r);
    o.g = clamp8(((A.g - B.g) * C.g + 0x80) / 256 + D.g);
    o.b = clamp8(((A.b - B.b) * C.b + 0x80) / 256 + D.b);
    o.a = clamp8(((Aa - Ba) * Ca + 0x80) / 256 + Da);
    return o;
}

static Col blend_cycle(int cyc, Col pix, Col mem, Col shade, bool final) {
    u32 pa = OM(cyc ? 28 : 30, 2), aa = OM(cyc ? 24 : 26, 2), ma = OM(cyc ? 20 : 22, 2), ba = OM(cyc ? 16 : 18, 2);
    Col blendc = col(R.blend), fogc = col(R.fog);
    Col P = pa == 0 ? pix : pa == 1 ? mem : pa == 2 ? blendc : fogc;
    Col M = ma == 0 ? pix : ma == 1 ? mem : ma == 2 ? blendc : fogc;
    int A = aa == 0 ? pix.a : aa == 1 ? fogc.a : aa == 2 ? shade.a : 0;
    int B = ba == 0 ? 255 - A : ba == 1 ? 255 : ba == 2 ? 255 : 0;
    // The final cycle blends only with force-blend on (no coverage here, so edges are not blended)
    if (final && !OM(14, 1)) return P;
    int den = A + B ? A + B : 1;
    Col o = {clamp8((P.r * A + M.r * B) / den), clamp8((P.g * A + M.g * B) / den), clamp8((P.b * A + M.b * B) / den), pix.a};
    return o;
}

// One pixel through the pipeline. Returns false when it is not written.
static void shade_pixel(u32 x, u32 y, int tile, Col shade, s32 s, s32 t, u32 z, bool useZ) {
    u32 cyc = OM(52, 2);
    if (useZ && OM(4, 1)) {  // Z compare
        u16 zold = z_read(x, y);
        u16 znew = (u16)(z >> 2);
        u32 zmode = OM(10, 2);
        if (zmode == 3) { if (znew > zold + 0x40) return; }  // decal: equal within a margin
        else if (znew > zold) return;
    }
    Col t0 = col(sample(tile, s, t)), t1 = col(sample(tile + 1, s, t));
    Col zero = {0, 0, 0, 0};
    Col c = combine_cycle(cyc == CYC2 ? 0 : 1, zero, t0, t1, shade);
    if (cyc == CYC2) c = combine_cycle(1, c, t1, t0, shade);
    if (OM(0, 1)) {  // alpha compare against the blend colour's alpha
        if (c.a <= (int)(R.blend & 0xFF)) return;
    }
    Col mem = col(fb_read(x, y));
    Col o;
    if (cyc == CYC2) { o = blend_cycle(0, c, mem, shade, false); o = blend_cycle(1, o, mem, shade, true); }
    else o = blend_cycle(0, c, mem, shade, true);
    fb_write(x, y, rgba(o.r, o.g, o.b, c.a));
    if (useZ && OM(5, 1)) z_write(x, y, (u16)(z >> 2));
}

// ---- Rectangles ---------------------------------------------------------------------------------------------------
static void clip_rect(int* x0, int* y0, int* x1, int* y1) {
    int sx0 = (int)R.sxh >> 2, sy0 = (int)R.syh >> 2, sx1 = (int)R.sxl >> 2, sy1 = (int)R.syl >> 2;
    if (*x0 < sx0) *x0 = sx0;
    if (*y0 < sy0) *y0 = sy0;
    if (*x1 > sx1) *x1 = sx1;
    if (*y1 > sy1) *y1 = sy1;
    if (*x1 > (int)R.ciWidth) *x1 = (int)R.ciWidth;
}

static void fill_rect(u64 w) {
    int xl = (int)((w >> 44) & 0xFFF), yl = (int)((w >> 32) & 0xFFF), xh = (int)((w >> 12) & 0xFFF), yh = (int)(w & 0xFFF);
    u32 cyc = OM(52, 2);
    int x0 = xh >> 2, y0 = yh >> 2, x1 = xl >> 2, y1 = yl >> 2;
    if (cyc == FILL || cyc == COPY) { x1++; y1++; }  // inclusive in fill / copy mode
    clip_rect(&x0, &y0, &x1, &y1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            if (cyc == FILL) {
                if (R.ciSize == 2) {
                    u16 p = (x & 1) ? (u16)R.fill : (u16)(R.fill >> 16);
                    m_w16(g_mem.rdram, (R.ciAddr + ((u32)y * R.ciWidth + (u32)x) * 2) & (g_mem.rdramSize - 1), p);
                } else if (R.ciSize == 3) {
                    m_w32(g_mem.rdram, (R.ciAddr + ((u32)y * R.ciWidth + (u32)x) * 4) & (g_mem.rdramSize - 1), R.fill);
                } else {
                    m_w8(g_mem.rdram, (R.ciAddr + (u32)y * R.ciWidth + (u32)x) & (g_mem.rdramSize - 1),
                         (u8)(R.fill >> (8 * (3 - (x & 3)))));
                }
            } else {
                Col shade = col(R.prim);
                shade_pixel((u32)x, (u32)y, 0, shade, 0, 0, R.primZ << 2, false);
            }
        }
    g_rdpStats.fills++;
}

static void tex_rect(const u64* w, bool flip) {
    int xl = (int)((w[0] >> 44) & 0xFFF), yl = (int)((w[0] >> 32) & 0xFFF);
    int tile = (int)((w[0] >> 24) & 7);
    int xh = (int)((w[0] >> 12) & 0xFFF), yh = (int)(w[0] & 0xFFF);
    s32 s0 = (s16)(w[1] >> 48), t0 = (s16)(w[1] >> 32);
    s32 dsdx = (s16)(w[1] >> 16), dtdy = (s16)w[1];  // s5.10
    u32 cyc = OM(52, 2);
    int x0 = xh >> 2, y0 = yh >> 2, x1 = xl >> 2, y1 = yl >> 2;
    if (cyc == COPY) dsdx >>= 2;  // copy mode: 4 texels a pixel step in the command
    if (cyc == FILL || cyc == COPY) { x1++; y1++; }
    int cx0 = x0, cy0 = y0;
    clip_rect(&x0, &y0, &x1, &y1);
    Col shade = col(R.prim);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int dx = x - cx0, dy = y - cy0;
            if (flip) { int tmp = dx; dx = dy; dy = tmp; }
            s32 s = s0 + ((dsdx * dx) >> 5), t = t0 + ((dtdy * dy) >> 5);  // 10.5 + s5.10 * pixels
            if (cyc == COPY) {
                u32 c = sample(tile, s, t);
                if (OM(0, 1) && (c & 0xFF) == 0) continue;
                fb_write((u32)x, (u32)y, c);
            } else {
                shade_pixel((u32)x, (u32)y, tile, shade, s, t, R.primZ << 2, false);
            }
        }
}

// ---- Triangles ----------------------------------------------------------------------------------------------------
static inline s32 sx14(u32 v) { return (s32)(v << 18) >> 18; }

static void triangle(const u64* w, bool shade, bool tex, bool zb) {
    u64 w0 = w[0];
    int lft = (int)((w0 >> 55) & 1), tile = (int)((w0 >> 48) & 7);
    s32 yl = sx14((u32)(w0 >> 32) & 0x3FFF), ym = sx14((u32)(w0 >> 16) & 0x3FFF), yh = sx14((u32)w0 & 0x3FFF);
    s32 xl = (s32)(w[1] >> 32), dxldy = (s32)w[1];
    s32 xh = (s32)(w[2] >> 32), dxhdy = (s32)w[2];
    s32 xm = (s32)(w[3] >> 32), dxmdy = (s32)w[3];
    const u64* p = w + 4;
    // Shade: RGBA, their x and edge derivatives (s15.16)
    s32 sc[4] = {0}, sdx[4] = {0}, sde[4] = {0};
    if (shade) {
        for (int k = 0; k < 4; k++) {
            int sh = 48 - 16 * k;
            sc[k] = (s32)(((u32)(p[0] >> sh) & 0xFFFF) << 16 | ((u32)(p[2] >> sh) & 0xFFFF));
            sdx[k] = (s32)(((u32)(p[1] >> sh) & 0xFFFF) << 16 | ((u32)(p[3] >> sh) & 0xFFFF));
            sde[k] = (s32)(((u32)(p[4] >> sh) & 0xFFFF) << 16 | ((u32)(p[6] >> sh) & 0xFFFF));
        }
        p += 8;
    }
    s32 tc[3] = {0}, tdx[3] = {0}, tde[3] = {0};
    if (tex) {
        for (int k = 0; k < 3; k++) {
            int sh = 48 - 16 * k;
            tc[k] = (s32)(((u32)(p[0] >> sh) & 0xFFFF) << 16 | ((u32)(p[2] >> sh) & 0xFFFF));
            tdx[k] = (s32)(((u32)(p[1] >> sh) & 0xFFFF) << 16 | ((u32)(p[3] >> sh) & 0xFFFF));
            tde[k] = (s32)(((u32)(p[4] >> sh) & 0xFFFF) << 16 | ((u32)(p[6] >> sh) & 0xFFFF));
        }
        p += 8;
    }
    s32 z0 = 0, dzdx = 0, dzde = 0;
    if (zb) {
        z0 = (s32)(p[0] >> 32);
        dzdx = (s32)p[0];
        dzde = (s32)(p[1] >> 32);
    }
    if (!shade) for (int k = 0; k < 4; k++) sc[k] = (s32)(((R.prim >> (24 - 8 * k)) & 255) << 16);
    int ytop = yh >> 2;
    int sx0 = (int)R.sxh >> 2, sy0 = (int)R.syh >> 2, sx1 = (int)R.sxl >> 2, sy1 = (int)R.syl >> 2;
    if (sx1 > (int)R.ciWidth) sx1 = (int)R.ciWidth;
    bool persp = OM(51, 1);
    for (int y = ytop; y * 4 < yl; y++) {
        if (y * 4 + 2 < yh) continue;  // the row's centre is above the triangle
        int dy = y - ytop;
        s32 xa = xh + dxhdy * dy;
        s32 xb = (y * 4 + 2 < ym) ? xm + dxmdy * dy : xl + dxldy * (y - (ym >> 2));
        s32 left = lft ? xa : xb, right = lft ? xb : xa;
        if (y < sy0 || y >= sy1) continue;
        int x0 = (int)((left + 0xFFFF) >> 16), x1 = (int)((right + 0xFFFF) >> 16);
        if (x0 < sx0) x0 = sx0;
        if (x1 > sx1) x1 = sx1;
        for (int x = x0; x < x1; x++) {
            s64 dx = ((s64)x << 16) - xa;  // from the major edge, s15.16
            Col c;
            int ch[4];
            for (int k = 0; k < 4; k++) ch[k] = clamp8((int)((sc[k] + (s64)sde[k] * dy + ((s64)sdx[k] * dx >> 16)) >> 16));
            c.r = ch[0]; c.g = ch[1]; c.b = ch[2]; c.a = ch[3];
            s32 s = 0, t = 0;
            if (tex) {
                s64 S = tc[0] + (s64)tde[0] * dy + ((s64)tdx[0] * dx >> 16);
                s64 T = tc[1] + (s64)tde[1] * dy + ((s64)tdx[1] * dx >> 16);
                if (persp) {
                    s64 W = tc[2] + (s64)tde[2] * dy + ((s64)tdx[2] * dx >> 16);
                    if (W <= 0) W = 1;
                    s = (s32)((S << 15) / W);  // S / W in 10.5 (W is 1.0 at 0x8000 in its integer part)
                    t = (s32)((T << 15) / W);
                } else {
                    s = (s32)(S >> 16);
                    t = (s32)(T >> 16);
                }
            }
            u32 z = 0;
            if (zb) {
                s64 Z = z0 + (s64)dzde * dy + ((s64)dzdx * dx >> 16);
                if (Z < 0) Z = 0;
                z = (u32)(Z >> 14);  // 18-bit
                if (z > 0x3FFFF) z = 0x3FFFF;
            }
            shade_pixel((u32)x, (u32)y, tile, c, s, t, OM(2, 1) ? (R.primZ << 2) : z, zb || OM(2, 1));
        }
    }
}

// ---- Texture loads ------------------------------------------------------------------------------------------------
static u8 ti_byte(u32 a) { return m_r8(g_mem.rdram, a & (g_mem.rdramSize - 1)); }

static void load_block(const u64 w) {
    int tile = (int)((w >> 24) & 7);
    u32 sl = (u32)((w >> 44) & 0xFFF), tl = (u32)((w >> 32) & 0xFFF), sh = (u32)((w >> 12) & 0xFFF), dxt = (u32)(w & 0xFFF);
    Tile* t = &R.tiles[tile];
    u32 bpp = R.tiSize == 3 ? 4 : R.tiSize == 2 ? 2 : 1;  // 4-bit textures load as 8-bit pairs
    u32 texels = sh - sl + 1;
    u32 bytes = R.tiSize == 0 ? (texels + 1) / 2 : texels * bpp;
    u32 src = R.tiAddr + (tl * R.tiWidth + sl) * bpp;
    u32 dst = t->tmem * 8;
    u32 tacc = 0;
    for (u32 word = 0; word * 8 < bytes; word++) {
        bool odd = (tacc >> 11) & 1;
        for (u32 b = 0; b < 8; b++) {
            u32 off = word * 8 + b;
            u8 v = ti_byte(src + off);
            if (R.tiSize == 3) {  // 32-bit: RG to the low half, BA to the high half
                u32 texel = off / 4, part = off & 3;
                u32 a = (dst + texel * 2 + (part & 1)) ^ (odd ? 4 : 0);
                R.tmem[((part < 2) ? 0 : 0x800) | (a & 0x7FF)] = v;
            } else {
                R.tmem[((dst + off) ^ (odd ? 4 : 0)) & 0xFFF] = v;
            }
        }
        tacc += dxt;
    }
}

static void load_tile(const u64 w) {
    int tile = (int)((w >> 24) & 7);
    u32 sl = (u32)((w >> 44) & 0xFFF) >> 2, tl = (u32)((w >> 32) & 0xFFF) >> 2;
    u32 sh = (u32)((w >> 12) & 0xFFF) >> 2, th = (u32)(w & 0xFFF) >> 2;
    Tile* t = &R.tiles[tile];
    t->sl = sl << 2; t->tl = tl << 2; t->sh = sh << 2; t->th = th << 2;
    u32 bpp = R.tiSize == 3 ? 4 : R.tiSize == 2 ? 2 : 1;
    for (u32 y = tl; y <= th; y++) {
        u32 row = y - tl;
        u32 swap = (row & 1) ? 4 : 0;
        for (u32 x = sl; x <= sh; x++) {
            u32 src = R.tiAddr + (y * R.tiWidth + x) * bpp;
            if (R.tiSize == 3) {
                u32 a = (t->tmem * 8 + row * t->line * 8 + (x - sl) * 2) ^ swap;
                R.tmem[a & 0x7FF] = ti_byte(src);
                R.tmem[(a + 1) & 0x7FF] = ti_byte(src + 1);
                R.tmem[0x800 | (a & 0x7FF)] = ti_byte(src + 2);
                R.tmem[0x800 | ((a + 1) & 0x7FF)] = ti_byte(src + 3);
            } else if (R.tiSize == 0) {
                if ((x - sl) & 1) continue;
                u32 a = (t->tmem * 8 + row * t->line * 8 + (x - sl) / 2) ^ swap;
                R.tmem[a & 0xFFF] = ti_byte(R.tiAddr + (y * R.tiWidth + x) / 2);
            } else {
                for (u32 b = 0; b < bpp; b++) {
                    u32 a = (t->tmem * 8 + row * t->line * 8 + (x - sl) * bpp + b) ^ swap;
                    R.tmem[a & 0xFFF] = ti_byte(src + b);
                }
            }
        }
    }
}

static void load_tlut(const u64 w) {
    int tile = (int)((w >> 24) & 7);
    u32 sl = (u32)((w >> 44) & 0xFFF) >> 2, sh = (u32)((w >> 12) & 0xFFF) >> 2;
    Tile* t = &R.tiles[tile];
    for (u32 i = sl; i <= sh && i < 256; i++) {
        u32 src = R.tiAddr + i * 2;
        u32 dst = t->tmem * 8 + (i - sl) * 8;
        for (u32 k = 0; k < 8; k += 2) {  // each entry fills a 64-bit word (the four banks)
            R.tmem[(dst + k) & 0xFFF] = ti_byte(src);
            R.tmem[(dst + k + 1) & 0xFFF] = ti_byte(src + 1);
        }
    }
}

// ---- Commands -----------------------------------------------------------------------------------------------------
void rdp_command(const u64* w, int n) {
    (void)n;
    u64 c = w[0];
    switch ((u32)(c >> 56) & 0x3F) {
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: {
        u32 id = (u32)(c >> 56) & 7;
        if (OM(52, 2) >= COPY && OM(52, 2) == FILL) break;  // triangles in fill mode: not drawn here
        triangle(w, id & 4, id & 2, id & 1);
        break;
    }
    case 0x24: tex_rect(w, false); break;
    case 0x25: tex_rect(w, true); break;
    case 0x2D:  // SET_SCISSOR
        R.sxh = (u32)(c >> 44) & 0xFFF; R.syh = (u32)(c >> 32) & 0xFFF;
        R.sxl = (u32)(c >> 12) & 0xFFF; R.syl = (u32)c & 0xFFF;
        break;
    case 0x2E: R.primZ = (u32)(c >> 16) & 0xFFFF; break;  // SET_PRIM_DEPTH
    case 0x2F: R.om = c; break;                            // SET_OTHER_MODES
    case 0x30: load_tlut(c); break;
    case 0x32: {  // SET_TILE_SIZE
        Tile* t = &R.tiles[(c >> 24) & 7];
        t->sl = (u32)(c >> 44) & 0xFFF; t->tl = (u32)(c >> 32) & 0xFFF;
        t->sh = (u32)(c >> 12) & 0xFFF; t->th = (u32)c & 0xFFF;
        break;
    }
    case 0x33: load_block(c); break;
    case 0x34: load_tile(c); break;
    case 0x35: {  // SET_TILE
        Tile* t = &R.tiles[(c >> 24) & 7];
        t->fmt = (u32)(c >> 53) & 7; t->size = (u32)(c >> 51) & 3; t->line = (u32)(c >> 41) & 0x1FF;
        t->tmem = (u32)(c >> 32) & 0x1FF; t->palette = (u32)(c >> 20) & 15;
        t->cmt = (u32)(c >> 18) & 3; t->maskt = (u32)(c >> 14) & 15; t->shiftt = (u32)(c >> 10) & 15;
        t->cms = (u32)(c >> 8) & 3; t->masks = (u32)(c >> 4) & 15; t->shifts = (u32)c & 15;
        break;
    }
    case 0x36: fill_rect(c); break;
    case 0x37: R.fill = (u32)c; break;
    case 0x38: R.fog = (u32)c; break;
    case 0x39: R.blend = (u32)c; break;
    case 0x3A: R.prim = (u32)c; R.primLod = (u32)(c >> 32) & 0xFF; R.primMinLod = (u32)(c >> 40) & 0x1F; break;
    case 0x3B: R.env = (u32)c; break;
    case 0x3C: R.combine = c; break;
    case 0x3D:  // SET_TEXTURE_IMAGE
        R.tiFmt = (u32)(c >> 53) & 7; R.tiSize = (u32)(c >> 51) & 3; R.tiWidth = ((u32)(c >> 32) & 0x3FF) + 1;
        R.tiAddr = (u32)c & 0x3FFFFFF;
        break;
    case 0x3E: R.zAddr = (u32)c & 0x3FFFFFF; break;
    case 0x3F:  // SET_COLOR_IMAGE
        R.ciFmt = (u32)(c >> 53) & 7; R.ciSize = (u32)(c >> 51) & 3; R.ciWidth = ((u32)(c >> 32) & 0x3FF) + 1;
        R.ciAddr = (u32)c & 0x3FFFFFF;
        break;
    default: break;  // syncs, keys, convert
    }
}
