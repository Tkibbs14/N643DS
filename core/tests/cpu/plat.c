#include "plat.h"

int g_failures;

#ifdef PLAT_ROM
// ISViewer window (sys/bus.c): bytes at 0x13FF0020, then the length at 0x13FF0014. 32-bit stores only, as the PI bus
// takes on hardware.
void plat_write(const char* s, u32 n) {
    volatile u32* buf = (volatile u32*)0xB3FF0020u;
    volatile u32* len = (volatile u32*)0xB3FF0014u;
    while (n) {
        u32 chunk = n > 512 ? 512 : n;
        for (u32 i = 0; i < chunk; i += 4) {
            u32 w = 0;
            for (u32 k = 0; k < 4; k++) w = (w << 8) | (i + k < chunk ? (u8)s[i + k] : 0);
            buf[i / 4] = w;
        }
        *len = chunk;
        s += chunk;
        n -= chunk;
    }
}

void plat_exit(int code) {
    (void)code;
    for (;;) {}
}
#else
static long sys3(long nr, long a, long b, long c) {
    register long v0 __asm__("$2") = nr;
    register long a0 __asm__("$4") = a;
    register long a1 __asm__("$5") = b;
    register long a2 __asm__("$6") = c;
    register long a3 __asm__("$7");
    __asm__ volatile("syscall" : "+r"(v0), "=r"(a3) : "r"(a0), "r"(a1), "r"(a2)
                     : "$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "hi", "lo", "memory");
    return v0;
}
void plat_write(const char* s, u32 n) { sys3(6001, 1, (long)s, n); }   // n32 write
void plat_exit(int code) { sys3(6058, code, 0, 0); for (;;) {} }        // n32 exit
#endif

void puts_(const char* s) {
    u32 n = 0;
    while (s[n]) n++;
    plat_write(s, n);
}

void put_hex(u64 v) {
    char b[16];
    for (int i = 0; i < 16; i++) b[i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
    plat_write(b, 16);
}

void put_dec(s64 v) {
    char b[24];
    int n = 0;
    u64 u = v < 0 ? (u64)-v : (u64)v;
    do { b[23 - n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) b[23 - n++] = '-';
    plat_write(b + 24 - n, (u32)n);
}

static const char* g_group;
static u64 g_hash, g_count;
#ifdef VERBOSE
static int g_verbose = 1;
#else
static int g_verbose;
#endif

void out_verbose(int on) { g_verbose = on; }
void out_begin(const char* group) { g_group = group; g_hash = 0xcbf29ce484222325ull; g_count = 0; }
void out(u64 v) {
    for (int i = 0; i < 8; i++) { g_hash ^= (v >> (8 * i)) & 0xFF; g_hash *= 0x100000001b3ull; }
    if (g_verbose) { puts_(g_group); puts_(" "); put_dec((s64)g_count); puts_(" "); put_hex(v); puts_("\n"); }
    g_count++;
}
void out_end(void) {
    puts_("OUT ");
    puts_(g_group);
    puts_(" n=");
    put_dec((s64)g_count);
    puts_(" hash=");
    put_hex(g_hash);
    puts_("\n");
}

void expect_(const char* what, int line, u64 got, u64 want) {
    if (!ON_ROM) return;
    if (got == want) return;
    g_failures++;
    puts_("FAIL ");
    puts_(what);
    puts_(" line ");
    put_dec(line);
    puts_(" got ");
    put_hex(got);
    puts_(" want ");
    put_hex(want);
    puts_("\n");
}

static u64 g_rng = 0x9E3779B97F4A7C15ull;
void rnd_seed(u64 s) { g_rng = s ? s : 1; }
u64 rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static const u64 kEdge[] = {0, 1, 2, 0x7FFFFFFFull, 0xFFFFFFFF80000000ull, 0xFFFFFFFFFFFFFFFFull,
                            0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFull, 0x80000000ull,
                            0x100000000ull, 0xFFFFFFFFFFFFFFFEull, 31, 32, 63, 64};

u64 val64(void) {
    u64 r = rnd();
    switch (r & 7) {
    case 0: return kEdge[(r >> 8) % (sizeof kEdge / sizeof kEdge[0])];
    case 1: return (u64)(s64)(s32)(u32)rnd();
    case 2: return rnd() & 0xFF;
    default: return rnd();
    }
}

u64 val32(void) {
    u64 r = rnd();
    u32 v;
    switch (r & 7) {
    case 0: { static const u32 e[] = {0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 31, 32};
              v = e[(r >> 8) & 7]; break; }
    case 1: v = (u32)(rnd() & 0xFF); break;
    default: v = (u32)rnd(); break;
    }
    return (u64)(s64)(s32)v;
}
