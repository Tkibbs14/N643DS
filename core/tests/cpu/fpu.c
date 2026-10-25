// COP1: arithmetic, compares and conversions in all four rounding modes, with FCR31's cause / flag bits. Operands and
// results stay normal: denormals raise the VR4300's unimplemented-operation exception (exc.c covers it), which a
// Linux program cannot take. Quiet-NaN arithmetic differs from IEEE on the VR4300 (an EXPECT below).
#include "plat.h"

#define N 400
#define FCR_MASK 0x0083F07Fu  // C, cause, flags, enables, rounding mode

static void set_fcr(u32 v) { __asm__ volatile("ctc1 %0, $31" : : "r"(v)); }
static u32 get_fcr(void) { u32 v; __asm__ volatile("cfc1 %0, $31" : "=r"(v)); return v; }

#define S2(fn, insn) \
    static u32 fn(u32 a, u32 b) { u32 r; \
        __asm__ volatile("mtc1 %1, $f0\n\tmtc1 %2, $f2\n\t" insn " $f4, $f0, $f2\n\tmfc1 %0, $f4" \
                         : "=r"(r) : "r"(a), "r"(b) : "$f0", "$f2", "$f4"); return r; }
#define S1(fn, insn) \
    static u32 fn(u32 a) { u32 r; \
        __asm__ volatile("mtc1 %1, $f0\n\t" insn " $f4, $f0\n\tmfc1 %0, $f4" : "=r"(r) : "r"(a) : "$f0", "$f4"); return r; }
#define D2(fn, insn) \
    static u64 fn(u64 a, u64 b) { u64 r; \
        __asm__ volatile("dmtc1 %1, $f0\n\tdmtc1 %2, $f2\n\t" insn " $f4, $f0, $f2\n\tdmfc1 %0, $f4" \
                         : "=r"(r) : "r"(a), "r"(b) : "$f0", "$f2", "$f4"); return r; }
#define D1(fn, insn) \
    static u64 fn(u64 a) { u64 r; \
        __asm__ volatile("dmtc1 %1, $f0\n\t" insn " $f4, $f0\n\tdmfc1 %0, $f4" : "=r"(r) : "r"(a) : "$f0", "$f4"); return r; }
// Conversions: source width in, destination width out (32-bit values through mtc1 / mfc1, 64-bit through dmtc1)
#define CV(fn, insn, inw, outw) \
    static u64 fn(u64 a) { u64 r; \
        __asm__ volatile(inw " %1, $f0\n\t" insn " $f4, $f0\n\t" outw " %0, $f4" : "=r"(r) : "r"(a) : "$f0", "$f4"); \
        return outw[0] == 'd' ? r : (u64)(u32)r; }

S2(add_s, "add.s") S2(sub_s, "sub.s") S2(mul_s, "mul.s") S2(div_s, "div.s")
S1(sqrt_s, "sqrt.s") S1(abs_s, "abs.s") S1(neg_s, "neg.s") S1(mov_s, "mov.s")
D2(add_d, "add.d") D2(sub_d, "sub.d") D2(mul_d, "mul.d") D2(div_d, "div.d")
D1(sqrt_d, "sqrt.d") D1(abs_d, "abs.d") D1(neg_d, "neg.d") D1(mov_d, "mov.d")
CV(cvt_s_d, "cvt.s.d", "dmtc1", "mfc1") CV(cvt_d_s, "cvt.d.s", "mtc1", "dmfc1")
CV(cvt_s_w, "cvt.s.w", "mtc1", "mfc1") CV(cvt_d_w, "cvt.d.w", "mtc1", "dmfc1")
CV(cvt_s_l, "cvt.s.l", "dmtc1", "mfc1") CV(cvt_d_l, "cvt.d.l", "dmtc1", "dmfc1")
CV(cvt_w_s, "cvt.w.s", "mtc1", "mfc1") CV(cvt_w_d, "cvt.w.d", "dmtc1", "mfc1")
CV(cvt_l_s, "cvt.l.s", "mtc1", "dmfc1") CV(cvt_l_d, "cvt.l.d", "dmtc1", "dmfc1")
CV(round_w_s, "round.w.s", "mtc1", "mfc1") CV(trunc_w_s, "trunc.w.s", "mtc1", "mfc1")
CV(ceil_w_s, "ceil.w.s", "mtc1", "mfc1") CV(floor_w_s, "floor.w.s", "mtc1", "mfc1")
CV(round_w_d, "round.w.d", "dmtc1", "mfc1") CV(trunc_w_d, "trunc.w.d", "dmtc1", "mfc1")
CV(ceil_w_d, "ceil.w.d", "dmtc1", "mfc1") CV(floor_w_d, "floor.w.d", "dmtc1", "mfc1")
CV(round_l_d, "round.l.d", "dmtc1", "dmfc1") CV(trunc_l_d, "trunc.l.d", "dmtc1", "dmfc1")
CV(ceil_l_d, "ceil.l.d", "dmtc1", "dmfc1") CV(floor_l_d, "floor.l.d", "dmtc1", "dmfc1")

// Normal values with exponents near 1 (results of + - * / stay normal), sometimes simple ones, signed zeros
static u32 fs(void) {
    u64 r = rnd();
    switch (r & 15) {
    case 0: return (r & 0x100) ? 0x80000000u : 0;
    case 1: { static const u32 k[] = {0x3F800000, 0x40400000, 0x3F000000, 0x3DCCCCCD, 0xBF800000, 0x41200000,
                                      0x4B000000, 0x3EAAAAAB};
              return k[(r >> 8) & 7]; }
    default: {
        u32 e = 127 - 24 + (u32)((r >> 8) % 49);
        return (u32)(r >> 32 & 0x80000000u) | (e << 23) | (u32)(rnd() & 0x7FFFFF);
    }
    }
}
static u64 fd(void) {
    u64 r = rnd();
    switch (r & 15) {
    case 0: return (r & 0x100) ? 0x8000000000000000ull : 0;
    case 1: { static const u64 k[] = {0x3FF0000000000000ull, 0x4008000000000000ull, 0x3FE0000000000000ull,
                                      0x3FB999999999999Aull, 0xBFF0000000000000ull, 0x4330000000000000ull};
              return k[((r >> 8) & 7) % 6]; }
    default: {
        u64 e = 1023 - 50 + ((r >> 8) % 101);
        return (r & 0x8000000000000000ull) | (e << 52) | (rnd() & 0xFFFFFFFFFFFFFull);
    }
    }
}

typedef u32 (*fs2)(u32, u32);
typedef u64 (*fd2)(u64, u64);

static void group_s2(const char* name, fs2 f) {
    out_begin(name);
    for (int rm = 0; rm < 4; rm++)
        for (int i = 0; i < N; i++) {
            u32 a = fs(), b = fs();
            set_fcr((u32)rm);
            out(f(a, b));
            out(get_fcr() & FCR_MASK);
        }
    out_end();
}
static void group_d2(const char* name, fd2 f) {
    out_begin(name);
    for (int rm = 0; rm < 4; rm++)
        for (int i = 0; i < N; i++) {
            u64 a = fd(), b = fd();
            set_fcr((u32)rm);
            out(f(a, b));
            out(get_fcr() & FCR_MASK);
        }
    out_end();
}
static void group_1(const char* name, u64 (*f)(u64), u64 (*gen)(void)) {
    out_begin(name);
    for (int rm = 0; rm < 4; rm++)
        for (int i = 0; i < N; i++) {
            u64 a = gen();
            set_fcr((u32)rm);
            out(f(a));
            out(get_fcr() & FCR_MASK);
        }
    out_end();
}

static u64 g_fs(void) { return fs(); }
static u64 g_fs_pos(void) { return fs() & 0x7FFFFFFFu; }
static u64 g_fd(void) { return fd(); }
static u64 g_fd_pos(void) { return fd() & 0x7FFFFFFFFFFFFFFFull; }
static u64 g_w(void) { return (u64)(u32)rnd(); }
static u64 g_l55(void) { return (u64)((s64)rnd() >> 10); }  // |v| < 2^54
// Integral-range floats for the float-to-integer conversions
static u64 g_fs_int(void) { u32 e = 127 + (u32)(rnd() % 30); return (u32)(rnd() & 0x80000000u) | (e << 23) | (u32)(rnd() & 0x7FFFFF); }
static u64 g_fs_small(void) { u32 e = 127 - 3 + (u32)(rnd() % 8); return (u32)(rnd() & 0x80000000u) | (e << 23) | (u32)(rnd() & 0x7FFFFF); }
static u64 g_fd_int(void) { u64 e = 1023 + rnd() % 30; return (rnd() & 0x8000000000000000ull) | (e << 52) | (rnd() & 0xFFFFFFFFFFFFFull); }
static u64 g_fd_l(void) { u64 e = 1023 + rnd() % 52; return (rnd() & 0x8000000000000000ull) | (e << 52) | (rnd() & 0xFFFFFFFFFFFFFull); }

static u64 w_sqrt_s(u64 a) { return sqrt_s((u32)a); }
static u64 w_abs_s(u64 a) { return abs_s((u32)a); }
static u64 w_neg_s(u64 a) { return neg_s((u32)a); }
static u64 w_mov_s(u64 a) { return mov_s((u32)a); }

// C.cond.S / .D: all sixteen conditions
static void cmp_s_all(int c, u32 a, u32 b);
static void cmp_d_all(int c, u64 a, u64 b);

static void compares(void) {
    static const u32 sv[] = {0x3F800000, 0x40000000, 0xBF800000, 0, 0x80000000, 0x7F800000, 0xFF800000,
                             0x7FBFFFFF /* quiet NaN (legacy MIPS) */, 0x7FC00000 /* signalling NaN */};
    static const u64 dv[] = {0x3FF0000000000000ull, 0x4000000000000000ull, 0xBFF0000000000000ull, 0,
                             0x8000000000000000ull, 0x7FF0000000000000ull, 0x7FF7FFFFFFFFFFFFull, 0x7FF8000000000000ull};
    out_begin("c.cond.s");
    for (int c = 0; c < 16; c++)
        for (unsigned i = 0; i < sizeof sv / 4; i++)
            for (unsigned j = 0; j < sizeof sv / 4; j++) {
                set_fcr(0);
                cmp_s_all(c, sv[i], sv[j]);
                out(get_fcr() & FCR_MASK);
            }
    out_end();
    out_begin("c.cond.d");
    for (int c = 0; c < 16; c++)
        for (unsigned i = 0; i < sizeof dv / 8; i++)
            for (unsigned j = 0; j < sizeof dv / 8; j++) {
                set_fcr(0);
                cmp_d_all(c, dv[i], dv[j]);
                out(get_fcr() & FCR_MASK);
            }
    out_end();
}

#define CMP_ONE_S(name, idx) if (c == idx) __asm__ volatile("mtc1 %0, $f0\n\tmtc1 %1, $f2\n\tc." #name ".s $f0, $f2" : : "r"(a), "r"(b) : "$f0", "$f2");
#define CMP_ONE_D(name, idx) if (c == idx) __asm__ volatile("dmtc1 %0, $f0\n\tdmtc1 %1, $f2\n\tc." #name ".d $f0, $f2" : : "r"(a), "r"(b) : "$f0", "$f2");
#define ALL(M) M(f,0) M(un,1) M(eq,2) M(ueq,3) M(olt,4) M(ult,5) M(ole,6) M(ule,7) M(sf,8) M(ngle,9) M(seq,10) \
               M(ngl,11) M(lt,12) M(nge,13) M(le,14) M(ngt,15)
static void cmp_s_all(int c, u32 a, u32 b) { ALL(CMP_ONE_S) }
static void cmp_d_all(int c, u64 a, u64 b) { ALL(CMP_ONE_D) }

// VR4300-specific (ares): a quiet NaN operand is an invalid operation; FS flushes a tiny result to zero.
static void vr4300(void) {
    set_fcr(0);
    u32 r = add_s(0x7FBFFFFF, 0x3F800000);
    EXPECT("qnan + 1 = default NaN", r, 0x7FBFFFFF);
    EXPECT("qnan + 1 raises V", (get_fcr() >> 12) & 63, 16);
    set_fcr(1u << 24);  // FS, round to nearest, no enables
    r = mul_s(0x0DA24260 /* 1e-30 */, 0x2EDBE6FF /* 1e-10 */);
    EXPECT("FS flushes a tiny product", r, 0);
    EXPECT("FS tiny raises U and I", (get_fcr() >> 12) & 63, 3);
    set_fcr(0);
}

int test_main(void) {
    rnd_seed(4);
    group_s2("add.s", add_s); group_s2("sub.s", sub_s); group_s2("mul.s", mul_s); group_s2("div.s", div_s);
    group_d2("add.d", add_d); group_d2("sub.d", sub_d); group_d2("mul.d", mul_d); group_d2("div.d", div_d);
    group_1("sqrt.s", w_sqrt_s, g_fs_pos); group_1("sqrt.d", sqrt_d, g_fd_pos);
    group_1("abs.s", w_abs_s, g_fs); group_1("neg.s", w_neg_s, g_fs); group_1("mov.s", w_mov_s, g_fs);
    group_1("abs.d", abs_d, g_fd); group_1("neg.d", neg_d, g_fd); group_1("mov.d", mov_d, g_fd);
    group_1("cvt.s.d", cvt_s_d, g_fd_int); group_1("cvt.d.s", cvt_d_s, g_fs);
    group_1("cvt.s.w", cvt_s_w, g_w); group_1("cvt.d.w", cvt_d_w, g_w);
    group_1("cvt.s.l", cvt_s_l, g_l55); group_1("cvt.d.l", cvt_d_l, g_l55);
    group_1("cvt.w.s", cvt_w_s, g_fs_int); group_1("cvt.w.s small", cvt_w_s, g_fs_small);
    group_1("cvt.w.d", cvt_w_d, g_fd_int); group_1("cvt.l.s", cvt_l_s, g_fs_int); group_1("cvt.l.d", cvt_l_d, g_fd_l);
    group_1("round.w.s", round_w_s, g_fs_small); group_1("trunc.w.s", trunc_w_s, g_fs_small);
    group_1("ceil.w.s", ceil_w_s, g_fs_small); group_1("floor.w.s", floor_w_s, g_fs_small);
    group_1("round.w.d", round_w_d, g_fd_int); group_1("trunc.w.d", trunc_w_d, g_fd_int);
    group_1("ceil.w.d", ceil_w_d, g_fd_int); group_1("floor.w.d", floor_w_d, g_fd_int);
    group_1("round.l.d", round_l_d, g_fd_l); group_1("trunc.l.d", trunc_l_d, g_fd_l);
    group_1("ceil.l.d", ceil_l_d, g_fd_l); group_1("floor.l.d", floor_l_d, g_fd_l);
    compares();
    vr4300();
    return 0;
}
