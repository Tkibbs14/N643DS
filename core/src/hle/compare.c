// HLE against the interpreter, task by task (n64emu --gfx compare): each graphics task runs through gfx.c from a
// snapshot of RDRAM and the RDP state, then again on the RSP interpreter from the same start, and the colour images
// either one drew to are compared pixel by pixel. The game continues with the interpreter's result, so both always
// start from the same input (comparing whole runs' last pictures does not work: HLE's task timing differs, and the
// games reach different frames).
// N64_HLE_DUMPDIR=dir writes the first N64_HLE_DUMPS (8) tasks with differences (from task N64_HLE_DUMPFROM) there as
// PPMs: HLE left, interpreter
// right.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sys/sys.h"
#include "hle.h"

bool g_hleCompare;
HleCompareStats g_hleCmp;
static u8 *g_before, *g_after;
static u8* g_rdpBefore;
static RdpCi g_hleCi[8];
static int g_hleCiN;
static u32 g_hleOps[64];
static u64 g_hleTri[16][22];
static int g_hleTriN;
static u64 g_hleTriOm[16][2];

bool hle_compare_before(void) {
    u32 size = g_mem.rdramSize, rdpSize;
    void* rdp = rdp_soft_state(&rdpSize);
    if (!g_before) {
        g_before = malloc(size);
        g_after = malloc(size);
        g_rdpBefore = malloc(rdpSize);
        if (!g_before || !g_after || !g_rdpBefore) { g_hleCompare = false; return false; }
    }
    memcpy(g_before, g_mem.rdram, size);
    memcpy(g_rdpBefore, rdp, rdpSize);
    rdp_ci_clear();
    memset(g_rdpOpCount, 0, sizeof g_rdpOpCount);
    g_rdpTriLogN = 0;
    const char* ti = getenv("N64_HLE_CMPTRI");
    g_rdpTriLogId = ti ? (u32)strtoul(ti, NULL, 16) : 0;
    if (!hle_gfx_task()) return false;  // unknown microcode: nothing drawn, nothing to undo
    g_hleCiN = g_rdpCiCount;
    memcpy(g_hleCi, g_rdpCi, sizeof g_hleCi);
    memcpy(g_hleOps, g_rdpOpCount, sizeof g_hleOps);
    memcpy(g_hleTri, g_rdpTriLog, sizeof g_hleTri);
    g_hleTriN = g_rdpTriLogN;
    memcpy(g_hleTriOm, g_rdpTriLogOm, sizeof g_hleTriOm);
    g_rdpTriLogN = 0;
    memset(g_rdpOpCount, 0, sizeof g_rdpOpCount);
    memcpy(g_after, g_mem.rdram, size);
    memcpy(g_mem.rdram, g_before, size);
    memcpy(rdp, g_rdpBefore, rdpSize);
    rdp_ci_clear();
    return true;
}

static u32 pixel_rgb(const u8* mem, const RdpCi* ci, u32 i) {  // 0xRRGGBB
    u32 mask = g_mem.rdramSize - 1;
    if (ci->size == 2) {
        u16 p = m_r16(mem, (ci->addr + i * 2) & mask);
        u32 r = (p >> 11) & 31, g = (p >> 6) & 31, b = (p >> 1) & 31;
        return ((r << 19 | r << 14) & 0xFF0000) | ((g << 11 | g << 6) & 0xFF00) | ((b << 3 | b >> 2) & 0xFF);
    }
    if (ci->size == 3) return m_r32(mem, (ci->addr + i * 4) & mask) >> 8;
    u8 v = m_r8(mem, (ci->addr + i) & mask);
    return (u32)v * 0x010101u;
}

static void dump(const RdpCi* ci, u32 w, u32 h) {
    const char* dir = getenv("N64_HLE_DUMPDIR");
    if (!dir) return;
    const char* n = getenv("N64_HLE_DUMPS");
    if (g_hleCmp.dumps >= (u64)(n ? atoi(n) : 8)) return;
    const char* from = getenv("N64_HLE_DUMPFROM");  // the first task to dump (0)
    if (from && g_hleCmp.tasks < strtoull(from, NULL, 10)) return;
    char path[512];
    snprintf(path, sizeof path, "%s/task%05llu_%06x.ppm", dir, (unsigned long long)g_hleCmp.tasks, (unsigned)ci->addr);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)(w * 2 + 4), (unsigned)h);
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w * 2 + 4; x++) {
            u32 c = x < w ? pixel_rgb(g_after, ci, y * ci->width + x)
                  : x >= w + 4 ? pixel_rgb(g_mem.rdram, ci, y * ci->width + x - w - 4) : 0xFF00FF;
            fputc((int)(c >> 16), f); fputc((int)(c >> 8) & 255, f); fputc((int)c & 255, f);
        }
    }
    fclose(f);
    g_hleCmp.dumps++;
}

void hle_compare_after(void) {
    RdpCi all[16];
    int n = 0;
    for (int k = 0; k < g_hleCiN; k++) all[n++] = g_hleCi[k];
    for (int k = 0; k < g_rdpCiCount; k++) {
        int j = 0;
        while (j < n && all[j].addr != g_rdpCi[k].addr) j++;
        if (j == n) all[n++] = g_rdpCi[k];
        else if (g_rdpCi[k].height > all[j].height) all[j].height = g_rdpCi[k].height;
    }
    u32 zAddr = rdp_z_addr(), mask = g_mem.rdramSize - 1;
    bool log = getenv("N64_HLE_CMPLOG") != NULL;
    u64 diff = 0, big = 0, pixels = 0, zDiff = 0, zPixels = 0;
    for (int k = 0; k < n; k++) {
        const RdpCi* ci = &all[k];
        u32 w = ci->width, h = ci->height ? ci->height : 240;
        if (w > 1024 || h > 1024) continue;
        u64 d = 0, b = 0;
        if (ci->addr == zAddr && ci->size == 2) {
            // The depth buffer (cleared as a colour image): 14-bit floating-point depth and 2-bit dz; HLE's float
            // arithmetic is allowed a few units of the 14-bit value
            for (u32 i = 0; i < w * h; i++) {
                int a = m_r16(g_after, (ci->addr + i * 2) & mask) >> 2, c = m_r16(g_mem.rdram, (ci->addr + i * 2) & mask) >> 2;
                if (abs(a - c) > 4) d++;
            }
            zPixels += (u64)w * h;
            zDiff += d;
            if (log && w >= 320 && h > 200) {
                u32 i = 200 * w + 160;
                fprintf(stderr, "  depth at (160, 200): hle %04x, interpreter %04x\n", (unsigned)m_r16(g_after, (ci->addr + i * 2) & mask),
                        (unsigned)m_r16(g_mem.rdram, (ci->addr + i * 2) & mask));
            }
            if (log) fprintf(stderr, "  depth %06x: %llu of %u differ by over 4\n", (unsigned)ci->addr, (unsigned long long)d, (unsigned)(w * h));
            continue;
        }
        for (u32 i = 0; i < w * h; i++) {
            u32 a = pixel_rgb(g_after, ci, i), c = pixel_rgb(g_mem.rdram, ci, i);
            if (a == c) continue;
            if (!d++ && log && ci->size == 2)
                fprintf(stderr, "  image %06x: first difference at (%u, %u): hle %04x, interpreter %04x\n", (unsigned)ci->addr, (unsigned)(i % w),
                        (unsigned)(i / w), (unsigned)m_r16(g_after, (ci->addr + i * 2) & mask),
                        (unsigned)m_r16(g_mem.rdram, (ci->addr + i * 2) & mask));
            int dr = (int)(a >> 16) - (int)(c >> 16), dg = (int)((a >> 8) & 255) - (int)((c >> 8) & 255), db = (int)(a & 255) - (int)(c & 255);
            if (abs(dr) > 24 || abs(dg) > 24 || abs(db) > 24) b++;
        }
        pixels += (u64)w * h;
        diff += d;
        big += b;
        if (b * 200 > (u64)w * h) dump(ci, w, h);  // more than 0.5% clearly different
    }
    g_hleCmp.tasks++;
    g_hleCmp.pixels += pixels;
    g_hleCmp.diff += diff;
    g_hleCmp.big += big;
    g_hleCmp.zPixels += zPixels;
    g_hleCmp.zDiff += zDiff;
    if (!diff) g_hleCmp.identical++;
    if (big * 100 > pixels) g_hleCmp.bad++;
    if (log)
        for (int k = 0; k < 64; k++)
            if (g_hleOps[k] != g_rdpOpCount[k])
                fprintf(stderr, "  rdp command %02x: hle %u, interpreter %u\n", k, (unsigned)g_hleOps[k], (unsigned)g_rdpOpCount[k]);
    if (log && getenv("N64_HLE_CMPTRI"))  // the first triangles of each, side by side: id lft, y h m l, x h m l
        for (int k = 0; k < 16 && (k < g_hleTriN || k < g_rdpTriLogN); k++)
            for (int side = 0; side < 2; side++) {
                const u64* w = side ? g_rdpTriLog[k] : g_hleTri[k];
                if (k >= (side ? g_rdpTriLogN : g_hleTriN)) continue;
                fprintf(stderr, "  tri %2d %s: %02x %d  y %.2f %.2f %.2f  x %.2f %.2f %.2f  slopes %.3f %.3f %.3f\n", k,
                        side ? "int" : "hle", (unsigned)(w[0] >> 56) & 0x3F, (int)(w[0] >> 55) & 1,
                        ((s32)((u32)w[0] << 18) >> 18) / 4.0, ((s32)((u32)(w[0] >> 16) << 18) >> 18) / 4.0,
                        ((s32)((u32)(w[0] >> 32) << 18) >> 18) / 4.0, (s32)(w[2] >> 32) / 65536.0, (s32)(w[3] >> 32) / 65536.0,
                        (s32)(w[1] >> 32) / 65536.0, (s32)w[2] / 65536.0, (s32)w[3] / 65536.0, (s32)w[1] / 65536.0);
                u32 id = (u32)(w[0] >> 56) & 0x3F;
                int b = 4 + ((id & 4) ? 8 : 0);
                if (id & 4)
                    fprintf(stderr, "         shade %d %d %d %d", (s16)(w[4] >> 48), (s16)(w[4] >> 32), (s16)(w[4] >> 16), (s16)w[4]);
                if (id & 2)
                    fprintf(stderr, "  stw %d %d %d  d/dx %d %d %d", (s16)(w[b] >> 48), (s16)(w[b] >> 32), (s16)(w[b] >> 16),
                            (s16)(w[b + 1] >> 48), (s16)(w[b + 1] >> 32), (s16)(w[b + 1] >> 16));
                int zb = b + ((id & 2) ? 8 : 0);
                const u64* om = side ? g_rdpTriLogOm[k] : g_hleTriOm[k];
                fprintf(stderr, "  om %014llx cc %016llx", (unsigned long long)om[0], (unsigned long long)om[1]);
                if (id & 1) fprintf(stderr, "  z %.1f dzdx %.3f", (s32)(w[zb] >> 32) / 65536.0, (s32)w[zb] / 65536.0);
                fprintf(stderr, "\n");
            }
    if (log)
        fprintf(stderr, "cmp task %llu: %d images, %llu of %llu pixels differ, %llu clearly\n", (unsigned long long)g_hleCmp.tasks - 1,
                n, (unsigned long long)diff, (unsigned long long)pixels, (unsigned long long)big);
}
