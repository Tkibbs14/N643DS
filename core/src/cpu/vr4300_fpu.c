// VR4300 floating point (COP1). Arithmetic runs on the host's IEEE unit under the guest's rounding mode; the VR4300's
// own rules wrap it: denormal operands and results raise the unimplemented-operation exception (unless FCR31.FS
// flushes results), NaNs use the MIPS legacy encoding (a set top mantissa bit means signalling) and results that are
// NaN become the default NaN (0x7FBFFFFF / 0x7FF7FFFFFFFFFFFF). These follow ares and n64-systemtest; the FPU test
// ROMs are the check (docs/TEST_ROMS.md).
//
// Built with -frounding-math -ffp-contract=off (Makefile) so the compiler neither folds nor fuses across fesetround.
#include <fenv.h>
#include <math.h>
#include <string.h>
#include "vr4300.h"

#define FS_ ((op >> 11) & 31)
#define FT_ ((op >> 16) & 31)
#define FD_ ((op >> 6) & 31)
#define FMT ((op >> 21) & 31)

enum { FMT_S = 16, FMT_D = 17, FMT_W = 20, FMT_L = 21 };
// FCR31 cause / flag / enable bit positions: inexact, underflow, overflow, division by zero, invalid, unimplemented
enum { F_I = 1, F_U = 2, F_O = 4, F_Z = 8, F_V = 16, F_E = 32 };
#define FCR31_C (1u << 23)
#define FCR31_FS (1u << 24)

static inline float u2f(u32 v) { float f; memcpy(&f, &v, 4); return f; }
static inline u32 f2u(float f) { u32 v; memcpy(&v, &f, 4); return v; }
static inline double u2d(u64 v) { double d; memcpy(&d, &v, 8); return d; }
static inline u64 d2u(double d) { u64 v; memcpy(&v, &d, 8); return v; }

// ---- Register file views ------------------------------------------------------------------------------------------
static inline bool fr(const Vr* c) { return (c->cop0[C0_STATUS] & ST_FR) != 0; }

u64 vr_fpr_read_w(Vr* c, int r) {
    if (fr(c)) return (u32)c->fpr[r];
    return (u32)(c->fpr[r & ~1] >> ((r & 1) * 32));
}

void vr_fpr_write_w(Vr* c, int r, u32 v) {
    // FR=1: the upper word is kept (unverified on hardware; ares writes the low word only).
    if (fr(c)) { c->fpr[r] = (c->fpr[r] & 0xFFFFFFFF00000000ull) | v; return; }
    int sh = (r & 1) * 32;
    c->fpr[r & ~1] = (c->fpr[r & ~1] & ~(0xFFFFFFFFull << sh)) | ((u64)v << sh);
}

u64 vr_fpr_read_d(Vr* c, int r) { return fr(c) ? c->fpr[r] : c->fpr[r & ~1]; }
void vr_fpr_write_d(Vr* c, int r, u64 v) {
    if (fr(c)) c->fpr[r] = v;
    else c->fpr[r & ~1] = v;
}

// ---- Control registers --------------------------------------------------------------------------------------------
static bool fpe_check(Vr* c);

u32 vr_cfc1(Vr* c, int rd) {
    if (rd == 0) return 0x00000A00;  // FCR0: implementation 0x0A, revision 0
    if (rd == 31) return c->fcr31;
    return 0;
}

void vr_ctc1(Vr* c, int rd, u32 v) {
    if (rd != 31) return;
    c->fcr31 = v & 0x0183FFFF;
    fpe_check(c);  // a cause bit whose enable is set (or E) traps at once
}

// ---- Exception plumbing -------------------------------------------------------------------------------------------
static bool fpe_check(Vr* c) {
    u32 cause = (c->fcr31 >> 12) & 63;
    u32 en = ((c->fcr31 >> 7) & 31) | F_E;
    if (cause & en) {
        vr_exception(c, EXC_FPE, 0);
        return true;
    }
    return false;
}

// Begins an operation: clears Cause, sets the host's rounding mode, clears the host's flags.
static void op_begin(Vr* c) {
    static const int modes[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
    c->fcr31 &= ~(63u << 12);
    fesetround(UNLIKELY(g_plant == 3) ? FE_TONEAREST : modes[c->fcr31 & 3]);
    feclearexcept(FE_ALL_EXCEPT);
}

static void raise_cause(Vr* c, u32 bits) { c->fcr31 |= bits << 12; }

static u32 host_flags(void) {
    int e = fetestexcept(FE_ALL_EXCEPT);
    u32 f = 0;
    if (e & FE_INEXACT) f |= F_I;
    if (e & FE_UNDERFLOW) f |= F_U;
    if (e & FE_OVERFLOW) f |= F_O;
    if (e & FE_DIVBYZERO) f |= F_Z;
    if (e & FE_INVALID) f |= F_V;
    return f;
}

// Ends an operation: Cause into Flags (when it does not trap); true when the exception was taken.
static bool op_end(Vr* c) {
    fesetround(FE_TONEAREST);
    if (fpe_check(c)) return true;
    c->fcr31 |= ((c->fcr31 >> 12) & 31) << 2;
    return false;
}

// ---- Operand classification ---------------------------------------------------------------------------------------
static inline bool s_nan(u32 v) { return (v & 0x7F800000u) == 0x7F800000u && (v & 0x7FFFFF); }
static inline bool s_snan(u32 v) { return s_nan(v) && (v & 0x400000); }   // legacy MIPS: top bit set = signalling
static inline bool s_denorm(u32 v) { return (v & 0x7F800000u) == 0 && (v & 0x7FFFFF); }
static inline bool d_nan(u64 v) { return (v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (v & 0xFFFFFFFFFFFFFull); }
static inline bool d_snan(u64 v) { return d_nan(v) && (v & 0x8000000000000ull); }
static inline bool d_denorm(u64 v) { return (v & 0x7FF0000000000000ull) == 0 && (v & 0xFFFFFFFFFFFFFull); }
#define S_DEFAULT_NAN 0x7FBFFFFFu
#define D_DEFAULT_NAN 0x7FF7FFFFFFFFFFFFull

// Input rule (ares): a denormal or signalling NaN operand is unimplemented; a quiet NaN is an invalid operation.
static bool in_s(Vr* c, u32 v) {
    if (s_denorm(v) || s_snan(v)) { raise_cause(c, F_E); return false; }
    if (s_nan(v)) raise_cause(c, F_V);
    return true;
}
static bool in_d(Vr* c, u64 v) {
    if (d_denorm(v) || d_snan(v)) { raise_cause(c, F_E); return false; }
    if (d_nan(v)) raise_cause(c, F_V);
    return true;
}

// Output rule: a tiny result is unimplemented unless FS flushes it (with U and I disabled); NaN becomes the default.
static bool out_s(Vr* c, u32* v) {
    u32 hf = host_flags();
    bool tiny = s_denorm(*v) || ((hf & F_U) && (*v & 0x7FFFFFFFu) == 0);
    if (tiny) {
        u32 en = (c->fcr31 >> 7) & 31;
        if (!(c->fcr31 & FCR31_FS) || (en & (F_U | F_I))) { raise_cause(c, F_E); return false; }
        raise_cause(c, F_U | F_I);
        u32 sign = *v & 0x80000000u;
        int rm = c->fcr31 & 3;
        bool away = (rm == 2 && !sign) || (rm == 3 && sign);
        *v = sign | (away ? 0x00800000u : 0);
        return true;
    }
    raise_cause(c, hf & (F_I | F_O | F_Z | F_V));
    if (s_nan(*v)) *v = S_DEFAULT_NAN;
    return true;
}
static bool out_d(Vr* c, u64* v) {
    u32 hf = host_flags();
    bool tiny = d_denorm(*v) || ((hf & F_U) && (*v & 0x7FFFFFFFFFFFFFFFull) == 0);
    if (tiny) {
        u32 en = (c->fcr31 >> 7) & 31;
        if (!(c->fcr31 & FCR31_FS) || (en & (F_U | F_I))) { raise_cause(c, F_E); return false; }
        raise_cause(c, F_U | F_I);
        u64 sign = *v & 0x8000000000000000ull;
        int rm = c->fcr31 & 3;
        bool away = (rm == 2 && !sign) || (rm == 3 && sign);
        *v = sign | (away ? 0x0010000000000000ull : 0);
        return true;
    }
    raise_cause(c, hf & (F_I | F_O | F_Z | F_V));
    if (d_nan(*v)) *v = D_DEFAULT_NAN;
    return true;
}

// ---- Conversions to integers --------------------------------------------------------------------------------------
// NaN, infinity or a value outside the target range is unimplemented on the VR4300 (ares), not an invalid operation.
static bool to_int(Vr* c, double x, int mode, bool is64, u64* out) {
    if (isnan(x) || isinf(x)) { raise_cause(c, F_E); return false; }
    double r;
    switch (mode) {
    case 0: r = nearbyint(x); break;  // CVT: the current rounding mode (fesetround in op_begin)
    case 1: { int save = fegetround(); fesetround(FE_TONEAREST); r = nearbyint(x); fesetround(save); break; }  // ROUND
    case 2: r = trunc(x); break;
    case 3: r = ceil(x); break;
    default: r = floor(x); break;
    }
    if (is64) {
        if (r >= 9223372036854775808.0 || r < -9223372036854775808.0) { raise_cause(c, F_E); return false; }
        *out = (u64)(s64)r;
    } else {
        if (r >= 2147483648.0 || r < -2147483648.0) { raise_cause(c, F_E); return false; }
        *out = (u64)(u32)(s32)r;
    }
    if (r != x) raise_cause(c, F_I);
    return true;
}

// ---- Conversions from integers ------------------------------------------------------------------------------------
// Rounded here in the guest's mode rather than by the host: on the 3DS, (float)int64 goes through a library routine
// that ignores the rounding mode (the 2DS's cvt.s.l results differed from qemu's, relay job 1437).
static u64 int_to_fp(Vr* c, s64 v, bool dbl) {
    int mant = dbl ? 52 : 23, bias = dbl ? 1023 : 127, top = dbl ? 63 : 31;
    u64 sign = v < 0, u = sign ? (u64)0 - (u64)v : (u64)v;
    if (!u) return 0;
    int msb = 63;
    while (!(u >> msb)) msb--;
    int exp = msb;
    u64 m;
    if (msb <= mant) {
        m = u << (mant - msb);
    } else {
        int sh = msb - mant;
        m = u >> sh;
        u64 rem = u & ((1ull << sh) - 1), half = 1ull << (sh - 1);
        bool up = false;
        switch (c->fcr31 & 3) {
        case 0: up = rem > half || (rem == half && (m & 1)); break;
        case 1: break;
        case 2: up = !sign && rem; break;
        default: up = sign && rem; break;
        }
        if (rem) raise_cause(c, F_I);
        if (up && ++m == (2ull << mant)) { m >>= 1; exp++; }
    }
    return (sign << top) | ((u64)(exp + bias) << mant) | (m & ((1ull << mant) - 1));
}

// ---- COP1 ---------------------------------------------------------------------------------------------------------
static void arith_s(Vr* c, u32 op, int fn) {
    u32 a = (u32)vr_fpr_read_w(c, FS_), b = (u32)vr_fpr_read_w(c, FT_);
    if (fn == 6) { vr_fpr_write_w(c, FD_, a); return; }  // MOV.S: no checks
    op_begin(c);
    bool two = fn <= 3;
    if (!in_s(c, a) || (two && !in_s(c, b))) { op_end(c); return; }
    volatile float x = u2f(a), y = u2f(b);
    float r;
    switch (fn) {
    case 0: r = x + y; break;
    case 1: r = x - y; break;
    case 2: r = x * y; break;
    case 3: r = x / y; break;
    case 4: r = sqrtf(x); break;
    case 5: r = fabsf(x); break;
    default: r = -x; break;  // 7: NEG
    }
    u32 v = f2u(r);
    if (!out_s(c, &v)) { op_end(c); return; }
    if (op_end(c)) return;
    vr_fpr_write_w(c, FD_, v);
}

static void arith_d(Vr* c, u32 op, int fn) {
    u64 a = vr_fpr_read_d(c, FS_), b = vr_fpr_read_d(c, FT_);
    if (fn == 6) { vr_fpr_write_d(c, FD_, a); return; }
    op_begin(c);
    bool two = fn <= 3;
    if (!in_d(c, a) || (two && !in_d(c, b))) { op_end(c); return; }
    volatile double x = u2d(a), y = u2d(b);
    double r;
    switch (fn) {
    case 0: r = x + y; break;
    case 1: r = x - y; break;
    case 2: r = x * y; break;
    case 3: r = x / y; break;
    case 4: r = sqrt(x); break;
    case 5: r = fabs(x); break;
    default: r = -x; break;
    }
    u64 v = d2u(r);
    if (!out_d(c, &v)) { op_end(c); return; }
    if (op_end(c)) return;
    vr_fpr_write_d(c, FD_, v);
}

static void compare(Vr* c, u32 op, bool dbl, int cond) {
    u64 a = dbl ? vr_fpr_read_d(c, FS_) : vr_fpr_read_w(c, FS_);
    u64 b = dbl ? vr_fpr_read_d(c, FT_) : vr_fpr_read_w(c, FT_);
    op_begin(c);
    bool nanA = dbl ? d_nan(a) : s_nan((u32)a), nanB = dbl ? d_nan(b) : s_nan((u32)b);
    bool snan = dbl ? (d_snan(a) || d_snan(b)) : (s_snan((u32)a) || s_snan((u32)b));
    bool less = false, equal = false, unordered = nanA || nanB;
    if (unordered) {
        if (snan || (cond & 8)) raise_cause(c, F_V);
    } else if (dbl) {
        double x = u2d(a), y = u2d(b);
        less = x < y;
        equal = x == y;
    } else {
        float x = u2f((u32)a), y = u2f((u32)b);
        less = x < y;
        equal = x == y;
    }
    bool result = ((cond & 4) && less) || ((cond & 2) && equal) || ((cond & 1) && unordered);
    if (op_end(c)) return;
    if (result) c->fcr31 |= FCR31_C;
    else c->fcr31 &= ~FCR31_C;
}

static void convert(Vr* c, u32 op, int fmt, int fn) {
    op_begin(c);
    u64 src = (fmt == FMT_S || fmt == FMT_W) ? vr_fpr_read_w(c, FS_) : vr_fpr_read_d(c, FS_);
    double x = 0;
    bool fromFloat = fmt == FMT_S || fmt == FMT_D;
    if (fmt == FMT_S) { if (!in_s(c, (u32)src)) { op_end(c); return; } x = u2f((u32)src); }
    else if (fmt == FMT_D) { if (!in_d(c, src)) { op_end(c); return; } x = u2d(src); }
    u64 out = 0;
    bool outD = false, ok = true;
    switch (fn) {
    case 0x08: case 0x09: case 0x0A: case 0x0B:  // ROUND / TRUNC / CEIL / FLOOR .L
        ok = fromFloat && to_int(c, x, 1 + (fn & 3), true, &out);
        if (!fromFloat) raise_cause(c, F_E);
        outD = true;
        break;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:  // ROUND / TRUNC / CEIL / FLOOR .W
        ok = fromFloat && to_int(c, x, 1 + (fn & 3), false, &out);
        if (!fromFloat) raise_cause(c, F_E);
        break;
    case 0x20: {  // CVT.S
        if (fmt == FMT_S) { raise_cause(c, F_E); ok = false; break; }
        u32 v;
        if (fmt == FMT_D) {
            volatile float r = (float)x;
            v = f2u(r);
        } else {
            // CVT from a 64-bit integer outside +-2^55 is unimplemented on the VR4300 (ares).
            s64 i = fmt == FMT_W ? (s64)(s32)(u32)src : (s64)src;
            if (i >= (1ll << 55) || i < -(1ll << 55)) { raise_cause(c, F_E); ok = false; break; }
            v = (u32)int_to_fp(c, i, false);
        }
        ok = out_s(c, &v);
        out = v;
        break;
    }
    case 0x21: {  // CVT.D
        if (fmt == FMT_D) { raise_cause(c, F_E); ok = false; break; }
        u64 v;
        if (fmt == FMT_S) {
            volatile double r = x;
            v = d2u(r);
        } else {
            s64 i = fmt == FMT_W ? (s64)(s32)(u32)src : (s64)src;
            if (i >= (1ll << 55) || i < -(1ll << 55)) { raise_cause(c, F_E); ok = false; break; }
            v = int_to_fp(c, i, true);
        }
        ok = out_d(c, &v);
        out = v;
        outD = true;
        break;
    }
    case 0x24: case 0x25:  // CVT.W / CVT.L
        if (!fromFloat) { raise_cause(c, F_E); ok = false; break; }
        ok = to_int(c, x, 0, fn == 0x25, &out);
        outD = fn == 0x25;
        break;
    default: raise_cause(c, F_E); ok = false; break;
    }
    if (op_end(c) || !ok) return;
    if (outD) vr_fpr_write_d(c, FD_, out);
    else vr_fpr_write_w(c, FD_, (u32)out);
}

void vr_cop1(Vr* c, u32 op) {
    int fmt = FMT;
    int rt = FT_, rd = FS_;
    switch (fmt) {
    case 0x00: c->gpr[rt] = SEXT32((u32)vr_fpr_read_w(c, rd)); return;  // MFC1
    case 0x01: c->gpr[rt] = vr_fpr_read_d(c, rd); return;               // DMFC1
    case 0x02: c->gpr[rt] = SEXT32(vr_cfc1(c, rd)); return;             // CFC1
    case 0x04: vr_fpr_write_w(c, rd, (u32)c->gpr[rt]); return;          // MTC1
    case 0x05: vr_fpr_write_d(c, rd, c->gpr[rt]); return;               // DMTC1
    case 0x06: vr_ctc1(c, rd, (u32)c->gpr[rt]); return;                 // CTC1
    case 0x08: {                                                        // BC1F / BC1T / BC1FL / BC1TL
        bool cbit = (c->fcr31 & FCR31_C) != 0;
        bool want = (op >> 16) & 1, likely = (op >> 17) & 1;
        bool taken = cbit == want;
        u64 target = c->pc + 4 + ((u64)(s64)(s16)(u16)op << 2);
        if (taken) {
            c->nextInDelay = true;
            c->branchPending = true;
            c->branchTo = target;
        } else if (likely) {
            c->npc += 4;
        } else {
            c->nextInDelay = true;
        }
        return;
    }
    }
    int fn = op & 63;
    if (fmt == FMT_S || fmt == FMT_D) {
        bool dbl = fmt == FMT_D;
        if (fn <= 7) {
            if (dbl) arith_d(c, op, fn);
            else arith_s(c, op, fn);
            return;
        }
        if (fn >= 0x30) { compare(c, op, dbl, fn & 15); return; }
        convert(c, op, fmt, fn);
        return;
    }
    if (fmt == FMT_W || fmt == FMT_L) {
        if (fn == 0x20 || fn == 0x21) { convert(c, op, fmt, fn); return; }
        op_begin(c);
        raise_cause(c, F_E);  // arithmetic on an integer format
        op_end(c);
        return;
    }
    vr_exception(c, EXC_RI, 0);
}
