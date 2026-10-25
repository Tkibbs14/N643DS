// N643DS on the 3DS: a cartridge menu (sdmc:/n643ds/roms), the CPU test cartridges (RomFS), and remote runs through the
// dev relay (relay.c, tools/relay.sh). The interpreter runs on core 0; the VI picture is copied to the top screen and
// speed is shown on the bottom one and written to sdmc:/n643ds/perf.csv (and the relay job's log).
//
// Relay job options: --rom NAME (a file in sdmc:/n643ds/roms whose name contains NAME), --seconds N (default 120),
// --tests (the CPU test cartridges), --show N (copy the picture every N fields, 0: never; default 2),
// --switch PATH (start another .3dsx, e.g. sdmc:/3ds/gc3ds.3dsx). Live commands: stop, shot.
#include <3ds.h>
#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "relay.h"
#include "../src/cpu/vr4300.h"
#include "../src/sys/rom.h"
#include "../src/sys/sys.h"
#include "../src/sys/system.h"

#define ROM_DIR "sdmc:/n643ds/roms"
#define PERF_PATH "sdmc:/n643ds/perf.csv"
#define RELAY_PENDING "sdmc:/n643ds/relay_pending.txt"

// The default 3dsx stack is 32 KB; the core keeps large buffers static, this is headroom.
u32 __stacksize__ = 256 * 1024;

static PrintConsole g_con;
static FILE* g_log;          // perf.csv, teed into the relay job when one runs
static bool g_quitApp;
static bool g_ranGame;       // a cartridge ran in this process (the next relay job restarts first, as gc3ds does)
static int g_showEvery = 2;  // copy the VI picture to the top screen every N fields
static volatile bool g_stopRun;
static char g_isvLine[512];
static size_t g_isvLen;
static int g_isvResult;      // -1 none, 0 PASS, 1 FAIL

static void logf_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_log) { vfprintf(g_log, fmt, ap); fflush(g_log); }
    va_end(ap);
}

// ---- Picture -------------------------------------------------------------------------------------------------------
// The VI framebuffer (RGBA5551 or RGBA8888 at VI_ORIGIN) scaled to fit 400x240 and written to the top screen
// (RGB565; the 3DS framebuffer is column-major, bottom row first).
static void show_picture(void) {
    u32 type = g_vi.ctrl & 3, w = g_vi.width;
    if (type < 2 || !w || w > 1024) return;
    u32 vs = (g_vi.vStart >> 16) & 0x3FF, ve = g_vi.vStart & 0x3FF;
    u32 lines = ve > vs ? (ve - vs) / 2 : 240;
    u32 yscale = g_vi.yScale & 0xFFF;
    u32 h = yscale ? lines * yscale / 1024 : 240;
    if (!h || h > 1024) h = 240;
    u16 width, height;
    u16* fb = (u16*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height);  // width = 240 (rows), height = 400
    u32 outW = w, outH = h;
    if (outW > 400 || outH > 240) {  // halve hi-res pictures
        outW = w / 2;
        outH = h / 2;
    }
    u32 x0 = (400 - (outW > 400 ? 400 : outW)) / 2, y0 = (240 - (outH > 240 ? 240 : outH)) / 2;
    u32 sx = w / outW;
    for (u32 y = 0; y < outH && y < 240; y++) {
        u32 srcY = y * h / outH;
        for (u32 x = 0; x < outW && x < 400; x++) {
            u32 srcX = x * sx;
            u16 o;
            if (type == 2) {
                u16 p = m_r16(g_mem.rdram, (g_vi.origin + (srcY * w + srcX) * 2) & (g_mem.rdramSize - 1));
                o = (u16)((p & 0xFFC0) | ((p >> 1) & 0x1F));  // RGBA5551 -> RGB565
            } else {
                u32 p = m_r32(g_mem.rdram, (g_vi.origin + (srcY * w + srcX) * 4) & (g_mem.rdramSize - 1));
                o = (u16)(((p >> 16) & 0xF800) | ((p >> 13) & 0x07E0) | ((p >> 11) & 0x001F));
            }
            fb[(x0 + x) * 240 + (239 - (y0 + y))] = o;
        }
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
}

// ---- Input ---------------------------------------------------------------------------------------------------------
static void read_pad(void) {
    hidScanInput();
    u32 k = hidKeysHeld();
    u16 b = 0;
    if (k & KEY_A) b |= 1u << 15;
    if (k & KEY_B) b |= 1u << 14;
    if (k & (KEY_ZL | KEY_ZR)) b |= 1u << 13;
    if (k & KEY_START) b |= 1u << 12;
    if (k & KEY_DUP) b |= 1u << 11;
    if (k & KEY_DDOWN) b |= 1u << 10;
    if (k & KEY_DLEFT) b |= 1u << 9;
    if (k & KEY_DRIGHT) b |= 1u << 8;
    if (k & KEY_L) b |= 1u << 5;
    if (k & KEY_R) b |= 1u << 4;
    if (k & (KEY_CSTICK_UP | KEY_X)) b |= 1u << 3;
    if (k & KEY_CSTICK_DOWN) b |= 1u << 2;
    if (k & (KEY_CSTICK_LEFT | KEY_Y)) b |= 1u << 1;
    if (k & KEY_CSTICK_RIGHT) b |= 1u << 0;
    circlePosition cp;
    hidCircleRead(&cp);  // about +-155
    int x = cp.dx * 80 / 150, y = cp.dy * 80 / 150;
    g_pad[0].buttons = b;
    g_pad[0].x = (s8)(x > 80 ? 80 : x < -80 ? -80 : x);
    g_pad[0].y = (s8)(y > 80 ? 80 : y < -80 ? -80 : y);
    g_pad[0].present = true;
}

// ---- Running a cartridge -------------------------------------------------------------------------------------------
static void on_isv(const char* s, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (s[i] == '\n' || g_isvLen == sizeof g_isvLine - 1) {
            g_isvLine[g_isvLen] = 0;
            printf("%s\n", g_isvLine);
            logf_("# isv %s\n", g_isvLine);
            if (g_isvResult < 0 && strstr(g_isvLine, "FAIL")) g_isvResult = 1;
            else if (g_isvResult < 0 && strstr(g_isvLine, "PASS")) g_isvResult = 0;
            if (g_isvResult >= 0) g_quit = true;
            g_isvLen = 0;
        } else {
            g_isvLine[g_isvLen++] = s[i];
        }
    }
}

static u64 g_fieldMark;
static void on_field(void) {
    if (g_showEvery && g_stats.fields % (u64)g_showEvery == 0) show_picture();
}

static void apply_cmds(void) {
    char cmd[128];
    while (relay_take_cmd(cmd, sizeof cmd)) {
        logf_("# command %s\n", cmd);
        if (!strcmp(cmd, "stop")) g_stopRun = true;
    }
}

// Runs a loaded cartridge for `seconds` of real time (0: until START+SELECT), logging speed every 10 s.
static const char* run_loaded(u32 seconds, bool interactive) {
    g_onIsv = on_isv;
    g_onField = on_field;
    g_stopRun = false;
    g_quit = false;
    g_isvResult = -1;
    g_isvLen = 0;
    sys_reset();
    rom_boot_hle();
    g_ranGame = true;
    logf_("# rom \"%s\" %.4s cic %d, entry %08lx\n", g_rom.name, g_rom.code, g_rom.cic, (unsigned long)g_rom.entry);
    logf_("t_s,fields,emu_s,speed_pct,mips\n");
    u64 t0 = svcGetSystemTick(), lastT = t0, lastFields = 0, lastIns = 0, lastCyc = 0;
    g_fieldMark = 0;
    for (;;) {
        u64 field = g_vi.cyclesPerField ? g_vi.cyclesPerField : CPU_HZ / 60;
        sys_run(g_cpu.cycles + field);
        if (g_quit || g_stopRun || !aptMainLoop()) break;
        if (interactive) {
            read_pad();
            if ((hidKeysHeld() & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) break;
        } else if (g_stats.fields % 30 == 0) {
            apply_cmds();
        }
        u64 now = svcGetSystemTick();
        if (now - lastT >= 10ull * SYSCLOCK_ARM11) {
            double dt = (double)(now - lastT) / SYSCLOCK_ARM11;
            double emu = (double)(g_cpu.cycles - lastCyc) / CPU_HZ;
            double mips = (double)(g_cpu.instructions - lastIns) / dt / 1e6;
            logf_("%.0f,%llu,%.3f,%.2f,%.2f\n", (double)(now - t0) / SYSCLOCK_ARM11, (unsigned long long)g_stats.fields,
                  (double)g_cpu.cycles / CPU_HZ, 100.0 * emu / dt, mips);
            consoleSelect(&g_con);
            printf("\x1b[1;1H%-20s\nspeed %5.1f%%  %5.1f MIPS\nfields %llu  gfx %llu  audio %llu    \n", g_rom.name,
                   100.0 * emu / dt, mips, (unsigned long long)g_stats.fields, (unsigned long long)g_stats.spTasks[1],
                   (unsigned long long)g_stats.spTasks[2]);
            lastT = now;
            lastFields = g_stats.fields;
            lastIns = g_cpu.instructions;
            lastCyc = g_cpu.cycles;
        }
        if (seconds && now - t0 >= (u64)seconds * SYSCLOCK_ARM11) break;
    }
    (void)lastFields;
    logf_("# end: %llu instructions, %llu fields, %.3f emulated s in %.1f s; pc %08lx\n",
          (unsigned long long)g_cpu.instructions, (unsigned long long)g_stats.fields, (double)g_cpu.cycles / CPU_HZ,
          (double)(svcGetSystemTick() - t0) / SYSCLOCK_ARM11, (unsigned long)(u32)g_cpu.pc);
    g_onIsv = NULL;
    g_onField = NULL;
    return "done";
}

static bool load_rom(const char* path) {
    if (!sys_init(8u << 20, SAVE_EEPROM4K)) { logf_("# out of memory for RDRAM\n"); return false; }
    if (!rom_load(path)) { logf_("# cannot load %s\n", path); return false; }
    return true;
}

// The CPU test cartridges (core/tests/cpu, built into RomFS by build.sh): each must print PASS.
static const char* run_tests(void) {
    static const char* const names[] = {"alu", "mem", "branch", "fpu", "exc", "cop0"};
    int failed = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char path[64];
        snprintf(path, sizeof path, "romfs:/tests/%s.z64", names[i]);
        logf_("# test %s\n", names[i]);
        printf("test %s...\n", names[i]);
        if (!load_rom(path)) { failed++; continue; }
        g_showEvery = 0;
        run_loaded(300, false);
        const char* r = g_isvResult == 0 ? "PASS" : g_isvResult == 1 ? "FAIL" : "NO RESULT";
        logf_("# test %s: %s\n", names[i], r);
        printf("test %s: %s\n", names[i], r);
        if (g_isvResult != 0) failed++;
    }
    logf_("# tests: %d failed\n", failed);
    return failed ? "failed" : "done";
}

// ---- ROM list ------------------------------------------------------------------------------------------------------
#define MAX_ROMS 64
static char g_roms[MAX_ROMS][128];
static int g_nroms;

static void scan_roms(void) {
    g_nroms = 0;
    DIR* d = opendir(ROM_DIR);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && g_nroms < MAX_ROMS) {
        const char* dot = strrchr(e->d_name, '.');
        if (dot && (!strcasecmp(dot, ".z64") || !strcasecmp(dot, ".n64") || !strcasecmp(dot, ".v64")))
            snprintf(g_roms[g_nroms++], sizeof g_roms[0], "%.127s", e->d_name);
    }
    closedir(d);
}

static int find_rom(const char* part) {
    for (int i = 0; i < g_nroms; i++)
        if (strcasestr(g_roms[i], part)) return i;
    return -1;
}

// ---- Restarting (hb:ldr), as gc3ds does (core/host3ds/main.c there: relaunch_into) ----------------------------------
static Result g_relaunchRc;
static const char* g_relaunchPath;
static volatile int g_relaunchDone;
static void relaunch_thread(void* arg) {
    (void)arg;
    Handle h;
    Result rc = svcConnectToPort(&h, "hb:ldr");
    if (R_SUCCEEDED(rc)) {
        const char* target = !strncmp(g_relaunchPath, "sdmc:", 5) ? g_relaunchPath + 5 : g_relaunchPath;
        static u32 argBuf[0x400 / 4];
        memset(argBuf, 0, sizeof argBuf);
        argBuf[0] = 1;
        snprintf((char*)&argBuf[1], sizeof argBuf - 4, "%s", g_relaunchPath);
        u32* cmd = getThreadCommandBuffer();
        cmd[0] = IPC_MakeHeader(2, 0, 2);
        cmd[1] = IPC_Desc_StaticBuffer(strlen(target) + 1, 0);
        cmd[2] = (u32)target;
        rc = svcSendSyncRequest(h);
        if (R_SUCCEEDED(rc)) rc = (Result)cmd[1];
        if (R_SUCCEEDED(rc)) {
            cmd = getThreadCommandBuffer();
            cmd[0] = IPC_MakeHeader(3, 0, 2);
            cmd[1] = IPC_Desc_StaticBuffer(sizeof argBuf, 1);
            cmd[2] = (u32)argBuf;
            rc = svcSendSyncRequest(h);
            if (R_SUCCEEDED(rc)) rc = (Result)cmd[1];
        }
        svcCloseHandle(h);
    }
    g_relaunchRc = rc;
    __sync_synchronize();
    g_relaunchDone = 1;
}
static bool relaunch_into(const char* path) {
    g_relaunchDone = 0;
    g_relaunchPath = path;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread t = threadCreate(relaunch_thread, NULL, 8 * 1024, prio, 1, true);
    if (!t) return false;
    u64 limit = svcGetSystemTick() + 3ull * SYSCLOCK_ARM11;
    while (!g_relaunchDone && svcGetSystemTick() < limit) svcSleepThread(10000000LL);
    if (!g_relaunchDone || R_FAILED(g_relaunchRc)) return false;
    aptSetChainloaderToSelf();
    return true;
}

// ---- Remote runs ---------------------------------------------------------------------------------------------------
static void relay_note(const char* msg) {
    FILE* t = relay_tee(NULL);
    if (t) { fprintf(t, "# %s\n", msg); fclose(t); }
}

static void save_pending(const RelayJob* j) {
    FILE* f = fopen(RELAY_PENDING, "w");
    if (f) { fprintf(f, "%u\n%s\n%s\n", j->id, j->build, j->args); fclose(f); }
}
static bool load_pending(RelayJob* j) {
    FILE* f = fopen(RELAY_PENDING, "r");
    if (!f) return false;
    char line[16], build[40];
    memset(j, 0, sizeof *j);
    bool ok = fgets(line, sizeof line, f) && fgets(build, sizeof build, f) && fgets(j->args, sizeof j->args, f);
    fclose(f);
    remove(RELAY_PENDING);
    j->id = (unsigned)strtoul(line, NULL, 10);
    build[strcspn(build, "\r\n")] = 0;
    snprintf(j->build, sizeof j->build, "%.16s", build);
    j->args[strcspn(j->args, "\r\n")] = 0;
    return ok && j->id;
}

static void restart_for_job(const char* selfPath, const char* why) {
    printf("%s\nRestarting...\n", why);
    bool ok = relaunch_into(selfPath);
    if (!ok) {
        char note[96];
        snprintf(note, sizeof note, "relay: automatic restart unavailable (%08lx): restart N643DS by hand",
                 (unsigned long)g_relaunchRc);
        relay_note(note);
        printf("Restart N643DS by hand.\n");
    }
    relay_flush();
    for (int i = 0; i < 60 && aptMainLoop(); i++) gspWaitForVBlank();
    g_quitApp = true;
}

static const char* opt_str(const char* args, const char* name, char* out, size_t n) {
    const char* p = strstr(args, name);
    if (!p) return NULL;
    p += strlen(name);
    while (*p == ' ') p++;
    size_t k = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && k + 1 < n) out[k++] = *p++;
    } else {
        while (*p && *p != ' ' && k + 1 < n) out[k++] = *p++;
    }
    out[k] = 0;
    return out;
}

static void remote_mode(const char* selfPath) {
    char err[256];
    consoleSelect(&g_con);
    consoleClear();
    printf("Remote runs: starting the network...\n");
    if (!relay_start(err, sizeof err)) { printf("relay: %s\n", err); svcSleepThread(3000000000LL); return; }
    const char* self = relay_self_id(selfPath);
    RelayJob job;
    bool have = load_pending(&job), fromPending = have;
    u64 lastPoll = 0;
    u32 polls = 0;
    for (;;) {
        if (!have) {
            if (!aptMainLoop()) { g_quitApp = true; return; }
            hidScanInput();
            if (hidKeysDown() & KEY_SELECT) return;
            if (lastPoll && svcGetSystemTick() - lastPoll < 10ull * SYSCLOCK_ARM11) { gspWaitForVBlank(); continue; }
            lastPoll = svcGetSystemTick();
            int r = relay_next(&job, err, sizeof err);
            consoleClear();
            printf("N643DS remote runs - build %s\nSELECT: menu\n\n", self);
            if (r < 0) { printf("relay error (retrying):\n%s\n", err); continue; }
            if (r == 0) { printf("waiting for jobs (%lu polls)\n", (unsigned long)++polls); continue; }
            have = true;
            fromPending = false;
        }
        have = false;
        printf("job %u: %s\n", job.id, job.args);
        relay_job_begin(job.id, fromPending ? RELAY_RESUME_SEQ : 0);
        char val[160];
        if (opt_str(job.args, "--switch", val, sizeof val)) {
            char note[200];
            bool ok = relaunch_into(val);
            snprintf(note, sizeof note, ok ? "relay: starting %s" : "relay: cannot start %s", val);
            relay_note(note);
            relay_job_end(ok ? "done" : "failed");
            if (ok) {
                for (int i = 0; i < 60 && aptMainLoop(); i++) gspWaitForVBlank();
                g_quitApp = true;
                return;
            }
            continue;
        }
        if (strcmp(job.build, "-") && strcmp(job.build, self)) {
            if (fromPending) {
                snprintf(err, sizeof err, "still build %s after the update to %s", self, job.build);
                relay_note(err);
                relay_job_end("failed");
                continue;
            }
            printf("downloading build %s...\n", job.build);
            if (!relay_fetch_build(job.build, selfPath, err, sizeof err)) {
                relay_note(err);
                relay_job_end("failed");
                continue;
            }
            save_pending(&job);
            restart_for_job(selfPath, "New build installed.");
            return;
        }
        if (g_ranGame) {
            save_pending(&job);
            restart_for_job(selfPath, "A fresh start for this job.");
            return;
        }
        FILE* plain = fopen(PERF_PATH, "w");
        g_log = relay_tee(plain);
        u32 seconds = 120;
        if (opt_str(job.args, "--seconds", val, sizeof val)) seconds = (u32)strtoul(val, NULL, 10);
        g_showEvery = 2;
        if (opt_str(job.args, "--show", val, sizeof val)) g_showEvery = atoi(val);
        logf_("# N643DS build %s, job %u: %s\n", self, job.id, job.args);
        const char* result = "failed";
        if (strstr(job.args, "--tests")) {
            result = run_tests();
        } else if (opt_str(job.args, "--rom", val, sizeof val)) {
            scan_roms();
            int ri = find_rom(val);
            char path[300];
            if (ri < 0) {
                logf_("# no ROM in " ROM_DIR " matches \"%s\"\n", val);
            } else {
                snprintf(path, sizeof path, ROM_DIR "/%s", g_roms[ri]);
                if (load_rom(path)) result = run_loaded(seconds, false);
            }
        } else {
            logf_("# nothing to do: give --rom NAME, --tests or --switch PATH\n");
        }
        fclose(g_log);
        g_log = NULL;
        relay_job_end(result);
        lastPoll = 0;
    }
}

// ---- Menu ----------------------------------------------------------------------------------------------------------
static void menu(const char* selfPath) {
    int sel = 0;
    for (;;) {
        scan_roms();
        int n = g_nroms + 2;
        consoleSelect(&g_con);
        consoleClear();
        printf("N643DS (development)\nA: start   SELECT: quit\nin a game: START+SELECT\n\n");
        for (int i = 0; i < n; i++) {
            const char* label = i < g_nroms ? g_roms[i] : i == g_nroms ? "[CPU tests]" : "[Remote runs]";
            printf("%c %.36s\n", i == sel ? '>' : ' ', label);
        }
        for (;;) {
            if (!aptMainLoop()) return;
            hidScanInput();
            u32 k = hidKeysDown();
            if (k & KEY_SELECT) return;
            if (k & KEY_DOWN) { sel = (sel + 1) % n; break; }
            if (k & KEY_UP) { sel = (sel + n - 1) % n; break; }
            if (k & KEY_A) {
                g_log = fopen(PERF_PATH, "w");
                if (sel < g_nroms) {
                    char path[300];
                    snprintf(path, sizeof path, ROM_DIR "/%s", g_roms[sel]);
                    consoleClear();
                    g_showEvery = 2;
                    if (load_rom(path)) run_loaded(0, true);
                } else if (sel == g_nroms) {
                    consoleClear();
                    run_tests();
                    printf("\nPress B\n");
                    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_B) break; gspWaitForVBlank(); }
                } else {
                    if (g_log) { fclose(g_log); g_log = NULL; }
                    remote_mode(selfPath);
                    if (g_quitApp) return;
                }
                if (g_log) { fclose(g_log); g_log = NULL; }
                break;
            }
            gspWaitForVBlank();
        }
    }
}

int main(int argc, char** argv) {
    const char* selfPath = argc > 0 && argv[0] && !strncmp(argv[0], "sdmc:/", 6) ? argv[0] : "sdmc:/3ds/n643ds.3dsx";
    osSetSpeedupEnable(true);
    gfxInitDefault();
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, false);
    consoleInit(GFX_BOTTOM, &g_con);
    romfsInit();
    mkdir("sdmc:/n643ds", 0777);
    relay_reserve();
    if (relay_auto() || access(RELAY_PENDING, F_OK) == 0) remote_mode(selfPath);
    if (!g_quitApp) menu(selfPath);
    relay_stop();
    romfsExit();
    gfxExit();
    return 0;
}
