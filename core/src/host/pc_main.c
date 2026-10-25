// PC frontend: runs a cartridge headless, prints ISViewer text, dumps VI pictures and statistics.
//   u64emu ROM [--fields N] [--cycles N] [--out DIR] [--shot-every N] [--trace N] [--save TYPE] [--4mb]
//              [--cic N] [--until-isv] [--stats]
// --until-isv stops at the first ISViewer line holding "PASS" or "FAIL" and exits 0 / 1 (the test ROMs' protocol).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "../cpu/vr4300.h"
#include "../sys/rom.h"
#include "../sys/sys.h"
#include "../sys/system.h"

static u64 g_maxFields = 600;
static const char* g_outDir;
static u64 g_shotEvery;
static bool g_untilIsv;
static int g_isvResult = -1;  // -1 none, 0 pass, 1 fail
static char g_isvLine[4096];
static size_t g_isvLen;

static void on_isv(const char* s, u32 n) {
    fwrite(s, 1, n, stdout);
    fflush(stdout);
    for (u32 i = 0; i < n; i++) {
        if (s[i] == '\n' || g_isvLen == sizeof g_isvLine - 1) {
            g_isvLine[g_isvLen] = 0;
            if (g_untilIsv && g_isvResult < 0) {
                if (strstr(g_isvLine, "FAIL")) g_isvResult = 1;
                else if (strstr(g_isvLine, "PASS")) g_isvResult = 0;
                if (g_isvResult >= 0) g_quit = true;
            }
            g_isvLen = 0;
        } else {
            g_isvLine[g_isvLen++] = s[i];
        }
    }
}

// The VI picture as a PPM: 16-bit (5551) or 32-bit framebuffer at VI_ORIGIN, VI_WIDTH pixels a line.
static void dump_picture(const char* path) {
    u32 type = g_vi.ctrl & 3, w = g_vi.width;
    if (type < 2 || !w || w > 1024) return;
    u32 vs = (g_vi.vStart >> 16) & 0x3FF, ve = g_vi.vStart & 0x3FF;
    u32 lines = ve > vs ? (ve - vs) / 2 : 240;
    u32 yscale = g_vi.yScale & 0xFFF;
    u32 h = yscale ? lines * yscale / 1024 : 240;
    if (!h || h > 1024) h = 240;
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            u8 rgb[3];
            if (type == 2) {
                u32 a = (g_vi.origin + (y * w + x) * 2) & (g_mem.rdramSize - 1);
                u16 p = m_r16(g_mem.rdram, a);
                rgb[0] = (u8)(((p >> 11) & 31) * 255 / 31);
                rgb[1] = (u8)(((p >> 6) & 31) * 255 / 31);
                rgb[2] = (u8)(((p >> 1) & 31) * 255 / 31);
            } else {
                u32 a = (g_vi.origin + (y * w + x) * 4) & (g_mem.rdramSize - 1);
                u32 p = m_r32(g_mem.rdram, a);
                rgb[0] = (u8)(p >> 24);
                rgb[1] = (u8)(p >> 16);
                rgb[2] = (u8)(p >> 8);
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static void on_field(void) {
    if (g_outDir && g_shotEvery && g_stats.fields % g_shotEvery == 0) {
        char path[512];
        snprintf(path, sizeof path, "%s/field_%06llu.ppm", g_outDir, (unsigned long long)g_stats.fields);
        dump_picture(path);
    }
    if (g_stats.fields >= g_maxFields) g_quit = true;
}

static int parse_save(const char* s) {
    if (!strcmp(s, "none")) return SAVE_NONE;
    if (!strcmp(s, "eep4k")) return SAVE_EEPROM4K;
    if (!strcmp(s, "eep16k")) return SAVE_EEPROM16K;
    if (!strcmp(s, "sram")) return SAVE_SRAM32K;
    if (!strcmp(s, "sram96k")) return SAVE_SRAM96K;
    if (!strcmp(s, "flash")) return SAVE_FLASH128K;
    fprintf(stderr, "unknown save type %s\n", s);
    exit(2);
}

int main(int argc, char** argv) {
    const char* romPath = NULL;
    u64 maxCycles = UINT64_MAX;
    u64 trace = 0;
    int saveType = SAVE_EEPROM4K, cic = 0;
    u32 rdram = 8u << 20;
    bool stats = false;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--fields") && more) g_maxFields = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--cycles") && more) maxCycles = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--out") && more) g_outDir = argv[++i];
        else if (!strcmp(a, "--shot-every") && more) g_shotEvery = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--trace") && more) trace = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--save") && more) saveType = parse_save(argv[++i]);
        else if (!strcmp(a, "--cic") && more) cic = atoi(argv[++i]);
        else if (!strcmp(a, "--4mb")) rdram = 4u << 20;
        else if (!strcmp(a, "--until-isv")) g_untilIsv = true;
        else if (!strcmp(a, "--stats")) stats = true;
        else if (a[0] != '-' && !romPath) romPath = a;
        else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    if (!romPath) {
        fprintf(stderr, "usage: u64emu ROM [--fields N] [--cycles N] [--out DIR] [--shot-every N] [--trace N]\n"
                        "              [--save none|eep4k|eep16k|sram|sram96k|flash] [--4mb] [--cic N] [--until-isv] [--stats]\n");
        return 2;
    }
    if (getenv("U64_PLANT")) g_plant = atoi(getenv("U64_PLANT"));
    if (g_outDir) mkdir(g_outDir, 0777);
    if (!sys_init(rdram, saveType)) return 1;
    if (!rom_load(romPath)) return 1;
    if (cic) rom_set_cic(cic);
    fprintf(stderr, "ROM \"%s\" %.4s v%u, entry 0x%08x, IPL3 CRC 0x%08x -> CIC %d%s, %s, RDRAM %u MB\n", g_rom.name,
            g_rom.code, g_rom.version, g_rom.entry, g_rom.ipl3Crc, g_rom.cic, g_rom.cicKnown ? "" : " (unknown, assumed)",
            g_mem.pal ? "PAL" : "NTSC", g_mem.rdramSize >> 20);
    g_onIsv = on_isv;
    g_onField = on_field;
    g_pad[0].present = true;
    sys_reset();
    rom_boot_hle();
    if (trace) vr_trace_enable(trace);

    clock_t t0 = clock();
    sys_run(maxCycles);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;

    if (g_outDir) {
        char path[512];
        snprintf(path, sizeof path, "%s/last.ppm", g_outDir);
        dump_picture(path);
    }
    if (stats || !g_untilIsv) {
        fprintf(stderr, "ran %llu instructions, %llu cycles (%.3f emulated s) in %.2f s; pc 0x%08x\n",
                (unsigned long long)g_cpu.instructions, (unsigned long long)g_cpu.cycles,
                (double)g_cpu.cycles / CPU_HZ, secs, (u32)g_cpu.pc);
        fprintf(stderr, "fields %llu, SP tasks gfx %llu audio %llu other %llu, PI DMAs %llu, SI DMAs %llu, AI buffers %llu\n",
                (unsigned long long)g_stats.fields, (unsigned long long)g_stats.spTasks[1],
                (unsigned long long)g_stats.spTasks[2],
                (unsigned long long)(g_stats.spTasks[0] + g_stats.spTasks[3] + g_stats.spTasks[4] + g_stats.spTasks[5] +
                                     g_stats.spTasks[6] + g_stats.spTasks[7]),
                (unsigned long long)g_stats.piDmas, (unsigned long long)g_stats.siDmas, (unsigned long long)g_stats.aiBufs);
    }
    if (g_untilIsv) {
        if (g_isvResult < 0) { fprintf(stderr, "no PASS / FAIL line before the limit\n"); return 3; }
        return g_isvResult;
    }
    return 0;
}
