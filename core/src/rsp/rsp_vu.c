// RSP vector unit (COP2): 32 registers of eight 16-bit lanes, a 48-bit accumulator per lane, the VCO / VCC / VCE flags,
// the reciprocal unit, and the vector loads and stores (LWC2 / SWC2). Semantics as documented on n64brew ("Reality
// Signal Processor / CPU Core", "RSP vector instructions") and in ares (rsp/vpu.cpp); PeterLemon's RSPTest/CP2 ROMs
// (hardware captures) check them (core/tools/peterlemon.py).
#include <string.h>
#include "rsp.h"

u8 rsp_dmem_r8(u32 a);
void rsp_dmem_w8(u32 a, u8 v);
#define DR(a) rsp_dmem_r8(a)
#define DW(a, v) rsp_dmem_w8((a), (u8)(v))

static u16 g_rcp[512], g_rsq[512];

void rsp_init_tables(void) {
    for (u32 i = 0; i < 512; i++) {
        u64 a = i + 512;
        u64 b = (1ull << 34) / a;
        g_rcp[i] = (u16)((b + 1) >> 8);
    }
    for (u32 i = 0; i < 512; i++) {
        u64 a = (i + 512) >> ((i % 2 == 1) ? 1 : 0);
        u64 b = 1ull << 17;
        while (a * (b + 1) * (b + 1) < (1ull << 44)) b++;  // the largest b with b < 1 / sqrt(a), scaled
        g_rsq[i] = (u16)(b >> 1);
    }
}

// ---- Register access -----------------------------------------------------------------------------------------------
static inline u8 vb(int r, int b) { u16 w = g_rsp.v[r][(b >> 1) & 7]; return (b & 1) ? (u8)w : (u8)(w >> 8); }
static inline void vbw(int r, int b, u8 x) {
    u16* w = &g_rsp.v[r][(b >> 1) & 7];
    *w = (b & 1) ? (u16)((*w & 0xFF00) | x) : (u16)((*w & 0x00FF) | (x << 8));
}

// The element selector: lane i of vt as operand e chooses it.
static inline int sel(int e, int i) {
    if (e < 2) return i;
    if (e < 4) return (i & ~1) | (e & 1);
    if (e < 8) return (i & ~3) | (e & 3);
    return e & 7;
}

static inline s64 sx48(s64 x) { return (s64)((u64)x << 16) >> 16; }
static inline s16 acch(int i) { return (s16)(g_rsp.acc[i] >> 32); }
static inline s16 accm(int i) { return (s16)(g_rsp.acc[i] >> 16); }
static inline u16 accl(int i) { return (u16)g_rsp.acc[i]; }
static inline void set_accl(int i, u16 v) { g_rsp.acc[i] = (g_rsp.acc[i] & ~0xFFFFll) | v; }

static inline u16 sclamp32(s32 v) { return v < -32768 ? 0x8000 : v > 32767 ? 0x7FFF : (u16)v; }
static inline u16 clamp_signed(int i) { return sclamp32((s32)(g_rsp.acc[i] >> 16)); }
// The low part, saturated by the signed value above it (VMADL, VMADN, VMUDN, VMUDL)
static inline u16 clamp_low(int i) {
    s32 hi = (s32)(g_rsp.acc[i] >> 16);
    return hi < -32768 ? 0 : hi > 32767 ? 0xFFFF : accl(i);
}
// VMULU / VMACU: negative -> 0, above 0x7FFF -> 0xFFFF
static inline u16 clamp_unsigned(int i) {
    s32 v = (s32)(g_rsp.acc[i] >> 16);
    return v < 0 ? 0 : v > 32767 ? 0xFFFF : (u16)v;
}

// ---- Vector operations ---------------------------------------------------------------------------------------------
static void vop(u32 op) {
    int e = (op >> 21) & 15, vt = (op >> 16) & 31, vs = (op >> 11) & 31, vd = (op >> 6) & 31;
    u16 s[8], t[8], d[8];
    for (int i = 0; i < 8; i++) { s[i] = g_rsp.v[vs][i]; t[i] = g_rsp.v[vt][sel(e, i)]; }
    Rsp* c = &g_rsp;
    int de = vs & 7;  // VMOV / VRCP family: the destination lane is in the vs field
    switch (op & 63) {
    case 0x00: case 0x01:  // VMULF / VMULU
        for (int i = 0; i < 8; i++) {
            c->acc[i] = sx48((s64)(s16)s[i] * (s16)t[i] * 2 + 0x8000);
            d[i] = (op & 1) ? clamp_unsigned(i) : clamp_signed(i);
        }
        break;
    case 0x02: case 0x0A:  // VRNDP / VRNDN
        for (int i = 0; i < 8; i++) {
            s64 p = (s16)t[i];
            if (vs & 1) p <<= 16;
            bool neg = c->acc[i] < 0;
            if ((op & 63) == 0x02 ? !neg : neg) c->acc[i] = sx48(c->acc[i] + p);
            d[i] = clamp_signed(i);
        }
        break;
    case 0x03:  // VMULQ
        for (int i = 0; i < 8; i++) {
            s32 p = (s16)s[i] * (s16)t[i];
            if (p < 0) p += 31;
            c->acc[i] = sx48((s64)p << 16);
            d[i] = sclamp32(p >> 1) & ~15;
        }
        break;
    case 0x0B:  // VMACQ
        for (int i = 0; i < 8; i++) {
            s32 p = (s32)(c->acc[i] >> 16);
            if (p < 0 && !(p & (1 << 5))) p += 32;
            else if (p >= 32 && !(p & (1 << 5))) p -= 32;
            c->acc[i] = sx48(((s64)p << 16) | accl(i));
            d[i] = sclamp32(p >> 1) & ~15;
        }
        break;
    case 0x04:  // VMUDL
        for (int i = 0; i < 8; i++) { c->acc[i] = ((u32)s[i] * t[i]) >> 16; d[i] = clamp_low(i); }
        break;
    case 0x05:  // VMUDM
        for (int i = 0; i < 8; i++) { c->acc[i] = sx48((s64)(s16)s[i] * t[i]); d[i] = clamp_signed(i); }
        break;
    case 0x06:  // VMUDN
        for (int i = 0; i < 8; i++) { c->acc[i] = sx48((s64)s[i] * (s16)t[i]); d[i] = clamp_low(i); }
        break;
    case 0x07:  // VMUDH
        for (int i = 0; i < 8; i++) { c->acc[i] = sx48((s64)((s32)(s16)s[i] * (s16)t[i]) << 16); d[i] = clamp_signed(i); }
        break;
    case 0x08: case 0x09:  // VMACF / VMACU
        for (int i = 0; i < 8; i++) {
            c->acc[i] = sx48(c->acc[i] + (s64)(s16)s[i] * (s16)t[i] * 2);
            d[i] = (op & 1) ? clamp_unsigned(i) : clamp_signed(i);
        }
        break;
    case 0x0C:  // VMADL
        for (int i = 0; i < 8; i++) { c->acc[i] = sx48(c->acc[i] + (((u32)s[i] * t[i]) >> 16)); d[i] = clamp_low(i); }
        break;
    case 0x0D:  // VMADM
        for (int i = 0; i < 8; i++) { c->acc[i] = sx48(c->acc[i] + (s64)(s16)s[i] * t[i]); d[i] = clamp_signed(i); }
        break;
    case 0x0E:  // VMADN
        for (int i = 0; i < 8; i++) { c->acc[i] = sx48(c->acc[i] + (s64)s[i] * (s16)t[i]); d[i] = clamp_low(i); }
        break;
    case 0x0F:  // VMADH
        for (int i = 0; i < 8; i++) {
            c->acc[i] = sx48(c->acc[i] + ((s64)((s32)(s16)s[i] * (s16)t[i]) << 16));
            d[i] = clamp_signed(i);
        }
        break;
    case 0x10: case 0x11:  // VADD / VSUB (with VCO's carry), saturating; VCO cleared
        for (int i = 0; i < 8; i++) {
            s32 carry = (c->vco >> i) & 1;
            s32 r = (op & 1) ? (s32)(s16)s[i] - (s16)t[i] - carry : (s32)(s16)s[i] + (s16)t[i] + carry;
            set_accl(i, (u16)r);
            d[i] = sclamp32(r);
        }
        c->vco = 0;
        break;
    case 0x13:  // VABS
        for (int i = 0; i < 8; i++) {
            s16 a = (s16)s[i], b = (s16)t[i];
            if (a < 0) {
                if (b == -32768) { set_accl(i, 0x8000); d[i] = 0x7FFF; }
                else { set_accl(i, (u16)-b); d[i] = (u16)-b; }
            } else if (a == 0) { set_accl(i, 0); d[i] = 0; }
            else { set_accl(i, (u16)b); d[i] = (u16)b; }
        }
        break;
    case 0x14:  // VADDC: unsigned add, carry into VCO
        c->vco = 0;
        for (int i = 0; i < 8; i++) {
            u32 r = (u32)s[i] + t[i];
            set_accl(i, (u16)r);
            d[i] = (u16)r;
            if (r >> 16) c->vco |= (u16)(1 << i);
        }
        break;
    case 0x15:  // VSUBC: unsigned subtract, borrow and not-equal into VCO
        c->vco = 0;
        for (int i = 0; i < 8; i++) {
            s32 r = (s32)s[i] - (s32)t[i];
            set_accl(i, (u16)r);
            d[i] = (u16)r;
            if (r < 0) c->vco |= (u16)(1 << i);
            if (r != 0) c->vco |= (u16)(0x100 << i);
        }
        break;
    case 0x1D:  // VSAR: read an accumulator slice
        for (int i = 0; i < 8; i++) d[i] = e == 8 ? (u16)acch(i) : e == 9 ? (u16)accm(i) : e == 10 ? accl(i) : 0;
        break;
    case 0x20: case 0x21: case 0x22: case 0x23: {  // VLT / VEQ / VNE / VGE
        u16 vcc = 0;
        for (int i = 0; i < 8; i++) {
            s16 a = (s16)s[i], b = (s16)t[i];
            bool carry = (c->vco >> i) & 1, ne = (c->vco >> (8 + i)) & 1, cond;
            switch (op & 63) {
            case 0x20: cond = a < b || (a == b && carry && ne); break;
            case 0x21: cond = a == b && !ne; break;
            case 0x22: cond = a != b || ne; break;
            default: cond = a > b || (a == b && (!carry || !ne)); break;
            }
            u16 r = cond ? s[i] : t[i];
            set_accl(i, r);
            d[i] = r;
            if (cond) vcc |= (u16)(1 << i);
        }
        c->vcc = vcc;
        c->vco = 0;
        break;
    }
    case 0x24: {  // VCL: clip test, low half (uses and updates VCC from the previous VCH)
        for (int i = 0; i < 8; i++) {
            u16 a = s[i], b = t[i];
            bool sign = (c->vco >> i) & 1, ne = (c->vco >> (8 + i)) & 1, ce = (c->vce >> i) & 1;
            bool le = (c->vcc >> i) & 1, ge = (c->vcc >> (8 + i)) & 1;
            u16 r;
            if (sign) {
                if (!ne) {
                    u32 sum = (u32)a + b;
                    bool carry = sum > 0xFFFF;
                    u16 low = (u16)sum;
                    le = ce ? (!low || !carry) : (!low && !carry);
                }
                r = le ? (u16)-b : a;
            } else {
                if (!ne) ge = (s32)a - (s32)b >= 0;
                r = ge ? b : a;
            }
            set_accl(i, r);
            d[i] = r;
            c->vcc = (u16)((c->vcc & ~(0x101 << i)) | (le << i) | (ge << (8 + i)));
        }
        c->vco = 0;
        c->vce = 0;
        break;
    }
    case 0x25: {  // VCH: clip test, high half
        u16 vco = 0, vcc = 0;
        u8 vce = 0;
        for (int i = 0; i < 8; i++) {
            s32 a = (s16)s[i], b = (s16)t[i];
            bool le, ge, sign, ne, ce;
            u16 r;
            if ((a ^ b) < 0) {
                s32 sum = a + b;
                sign = true;
                ge = b < 0;
                le = sum <= 0;
                ce = sum == -1;
                ne = sum != 0 && s[i] != (u16)(t[i] ^ 0xFFFF);
                r = le ? (u16)-b : (u16)a;
            } else {
                s32 diff = a - b;
                sign = false;
                le = b < 0;
                ge = diff >= 0;
                ce = false;
                ne = diff != 0 && s[i] != (u16)(t[i] ^ 0xFFFF);
                r = ge ? (u16)b : (u16)a;
            }
            set_accl(i, r);
            d[i] = r;
            vcc |= (u16)((le << i) | (ge << (8 + i)));
            vco |= (u16)((sign << i) | (ne << (8 + i)));
            vce |= (u8)(ce << i);
        }
        c->vcc = vcc;
        c->vco = vco;
        c->vce = vce;
        break;
    }
    case 0x26: {  // VCR: clip test, one's complement
        u16 vcc = 0;
        for (int i = 0; i < 8; i++) {
            s32 a = (s16)s[i], b = (s16)t[i];
            bool le, ge;
            u16 r;
            if ((a ^ b) < 0) {
                ge = b < 0;
                le = a + b + 1 <= 0;
                r = le ? (u16)~b : (u16)a;
            } else {
                le = b < 0;
                ge = a - b >= 0;
                r = ge ? (u16)b : (u16)a;
            }
            set_accl(i, r);
            d[i] = r;
            vcc |= (u16)((le << i) | (ge << (8 + i)));
        }
        c->vcc = vcc;
        c->vco = 0;
        c->vce = 0;
        break;
    }
    case 0x27:  // VMRG
        for (int i = 0; i < 8; i++) {
            u16 r = ((c->vcc >> i) & 1) ? s[i] : t[i];
            set_accl(i, r);
            d[i] = r;
        }
        c->vco = 0;
        break;
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D:  // VAND VNAND VOR VNOR VXOR VNXOR
        for (int i = 0; i < 8; i++) {
            u16 r;
            switch (op & 63) {
            case 0x28: r = s[i] & t[i]; break;
            case 0x29: r = (u16)~(s[i] & t[i]); break;
            case 0x2A: r = s[i] | t[i]; break;
            case 0x2B: r = (u16)~(s[i] | t[i]); break;
            case 0x2C: r = s[i] ^ t[i]; break;
            default: r = (u16)~(s[i] ^ t[i]); break;
            }
            set_accl(i, r);
            d[i] = r;
        }
        break;
    case 0x30: case 0x31: case 0x34: case 0x35: {  // VRCP / VRCPL / VRSQ / VRSQL
        bool low = (op & 1) != 0, rsq = (op & 4) != 0;
        u16 elem = g_rsp.v[vt][e & 7];
        s32 input = low && c->divDp ? (s32)(((u32)(u16)c->divIn << 16) | elem) : (s32)(s16)elem;
        s32 mask = input >> 31;
        s32 data = input ^ mask;
        if (input > -32768) data -= mask;
        s32 result;
        if (data == 0) {
            result = 0x7FFFFFFF;
        } else if (input == -32768) {
            result = (s32)0xFFFF0000;
        } else {
            u32 shift = (u32)__builtin_clz((u32)data);
            u32 index = (u32)(((u64)(u32)data << shift & 0x7FC00000u) >> 22);
            if (rsq) {
                result = g_rsq[(index & 0x1FE) | (shift & 1)];
                result = (0x10000 | result) << 14;
                result = (s32)((u32)result >> ((31 - shift) >> 1)) ^ mask;
            } else {
                result = g_rcp[index];
                result = (0x10000 | result) << 14;
                result = (s32)((u32)result >> (31 - shift)) ^ mask;
            }
        }
        c->divDp = false;
        c->divOut = (s16)(result >> 16);
        for (int i = 0; i < 8; i++) set_accl(i, t[i]);
        memcpy(d, g_rsp.v[vd], sizeof d);
        d[de] = (u16)result;
        break;
    }
    case 0x32: case 0x36:  // VRCPH / VRSQH: the high half of the next input; the last result's high half out
        for (int i = 0; i < 8; i++) set_accl(i, t[i]);
        c->divIn = (s16)g_rsp.v[vt][e & 7];
        c->divDp = true;
        memcpy(d, g_rsp.v[vd], sizeof d);
        d[de] = (u16)c->divOut;
        break;
    case 0x33:  // VMOV
        for (int i = 0; i < 8; i++) set_accl(i, t[i]);
        memcpy(d, g_rsp.v[vd], sizeof d);
        d[de] = t[de];
        break;
    case 0x37: case 0x3F:  // VNOP / VNULL: nothing changes (RSPTest/CP2/VNOP)
        return;
    default:  // reserved: the accumulator takes vs + vt, the destination is cleared (ares; RSPTest/CP2/RESERVED shows
              // VACC and others accumulating instead: not yet modelled, no game known to use them)
        for (int i = 0; i < 8; i++) { set_accl(i, (u16)(s[i] + t[i])); d[i] = 0; }
        break;
    }
    memcpy(g_rsp.v[vd], d, sizeof d);
}

void rsp_vu_cop2(u32 op) {
    if (op & (1u << 25)) { vop(op); return; }
    int rt = (op >> 16) & 31, rd = (op >> 11) & 31, e = (op >> 7) & 15;
    u32* r = g_rsp.r;
    switch ((op >> 21) & 31) {
    case 0x00:  // MFC2
        r[rt] = (u32)(s32)(s16)(((u16)vb(rd, e) << 8) | vb(rd, (e + 1) & 15));
        break;
    case 0x02:  // CFC2
        switch (rd & 3) {
        case 0: r[rt] = (u32)(s32)(s16)g_rsp.vco; break;
        case 1: r[rt] = (u32)(s32)(s16)g_rsp.vcc; break;
        default: r[rt] = g_rsp.vce; break;
        }
        break;
    case 0x04:  // MTC2
        vbw(rd, e, (u8)(r[rt] >> 8));
        if (e + 1 < 16) vbw(rd, e + 1, (u8)r[rt]);
        break;
    case 0x06:  // CTC2
        switch (rd & 3) {
        case 0: g_rsp.vco = (u16)r[rt]; break;
        case 1: g_rsp.vcc = (u16)r[rt]; break;
        default: g_rsp.vce = (u8)r[rt]; break;
        }
        break;
    }
    r[0] = 0;
}

// ---- Vector loads and stores ---------------------------------------------------------------------------------------
static inline s32 off7(u32 op) { return (s32)((op & 0x7F) ^ 0x40) - 0x40; }

void rsp_vu_lwc2(u32 op) {
    int vt = (op >> 16) & 31, e = (op >> 7) & 15, sub = (op >> 11) & 31;
    u32 base = g_rsp.r[(op >> 21) & 31];
    s32 off = off7(op);
    switch (sub) {
    case 0: case 1: case 2: case 3: {  // LBV LSV LLV LDV
        int n = 1 << sub;
        u32 a = base + (u32)(off * n);
        for (int i = 0; i < n && e + i < 16; i++) vbw(vt, e + i, DR(a + (u32)i));
        break;
    }
    case 4: {  // LQV: up to the end of the 16-byte line
        u32 a = base + (u32)(off * 16);
        int end = e + 16 - (int)(a & 15);
        if (end > 16) end = 16;
        for (int b = e; b < end; b++) vbw(vt, b, DR(a++));
        break;
    }
    case 5: {  // LRV: the part of the line before the address, into the vector's end
        u32 a = base + (u32)(off * 16);
        int start = 16 - ((int)(a & 15) - e);
        a &= ~15u;
        for (int b = start; b < 16; b++) vbw(vt, b & 15, DR(a++));
        break;
    }
    case 6: case 7: {  // LPV / LUV: packed bytes into lanes, << 8 / << 7
        u32 a = base + (u32)(off * 8);
        int index = (int)(a & 7) - e;
        a &= ~7u;
        for (int i = 0; i < 8; i++)
            g_rsp.v[vt][i] = (u16)(DR(a + (u32)((index + i) & 15)) << (sub == 6 ? 8 : 7));
        break;
    }
    case 8: {  // LHV: every other byte, << 7
        u32 a = base + (u32)(off * 16);
        int index = (int)(a & 7) - e;
        a &= ~7u;
        for (int i = 0; i < 8; i++) g_rsp.v[vt][i] = (u16)(DR(a + (u32)((index + i * 2) & 15)) << 7);
        break;
    }
    case 9: {  // LFV: every fourth byte, << 7, into half the vector
        u32 a = base + (u32)(off * 16);
        int index = (int)(a & 7) - e;
        a &= ~7u;
        u16 tmp[8];
        for (int i = 0; i < 4; i++) {
            tmp[i] = (u16)(DR(a + (u32)((index + i * 4) & 15)) << 7);
            tmp[i + 4] = (u16)(DR(a + (u32)((index + i * 4 + 8) & 15)) << 7);
        }
        int end = e + 8 > 16 ? 16 : e + 8;
        for (int b = e; b < end; b++) {
            u16 w = tmp[(b >> 1) & 7];
            vbw(vt, b, (b & 1) ? (u8)w : (u8)(w >> 8));
        }
        break;
    }
    case 11: {  // LTV: a transposed load into eight registers
        u32 a = base + (u32)(off * 16);
        u32 begin = a & ~7u;
        a = begin + (((u32)e + (a & 8)) & 15);
        int vbase = vt & ~7, voff = e >> 1;
        for (int i = 0; i < 8; i++) {
            vbw(vbase + voff, i * 2, DR(a++));
            if (a == begin + 16) a = begin;
            vbw(vbase + voff, i * 2 + 1, DR(a++));
            if (a == begin + 16) a = begin;
            voff = (voff + 1) & 7;
        }
        break;
    }
    default: break;
    }
}

void rsp_vu_swc2(u32 op) {
    int vt = (op >> 16) & 31, e = (op >> 7) & 15, sub = (op >> 11) & 31;
    u32 base = g_rsp.r[(op >> 21) & 31];
    s32 off = off7(op);
    switch (sub) {
    case 0: case 1: case 2: case 3: {  // SBV SSV SLV SDV
        int n = 1 << sub;
        u32 a = base + (u32)(off * n);
        for (int i = 0; i < n; i++) DW(a + (u32)i, vb(vt, (e + i) & 15));
        break;
    }
    case 4: {  // SQV
        u32 a = base + (u32)(off * 16);
        int end = e + 16 - (int)(a & 15);
        for (int b = e; b < end; b++) DW(a++, vb(vt, b & 15));
        break;
    }
    case 5: {  // SRV
        u32 a = base + (u32)(off * 16);
        int end = e + (int)(a & 15), shift = 16 - (int)(a & 15);
        a &= ~15u;
        for (int b = e; b < end; b++) DW(a++, vb(vt, (b + shift) & 15));
        break;
    }
    case 6: case 7: {  // SPV / SUV
        u32 a = base + (u32)(off * 8);
        for (int b = e; b < e + 8; b++) {
            bool packed = ((b & 15) < 8) == (sub == 6);
            if (packed) DW(a++, vb(vt, (b & 7) << 1));
            else DW(a++, g_rsp.v[vt][b & 7] >> 7);
        }
        break;
    }
    case 8: {  // SHV
        u32 a = base + (u32)(off * 16);
        int index = (int)(a & 7);
        a &= ~7u;
        for (int i = 0; i < 8; i++) {
            int b = e + i * 2;
            u8 value = (u8)((vb(vt, b & 15) << 1) | (vb(vt, (b + 1) & 15) >> 7));
            DW(a + (u32)((index + i * 2) & 15), value);
        }
        break;
    }
    case 9: {  // SFV: four lanes >> 7 to every fourth byte; the lanes depend on the element (ares)
        u32 a = base + (u32)(off * 16);
        int index = (int)(a & 7);
        a &= ~7u;
        static const s8 lanes[16][4] = {{0, 1, 2, 3}, {6, 7, 4, 5}, {-1}, {-1}, {1, 2, 3, 0}, {7, 4, 5, 6}, {-1}, {-1},
                                        {4, 5, 6, 7}, {-1}, {-1}, {3, 0, 1, 2}, {5, 6, 7, 4}, {-1}, {-1}, {0, 1, 2, 3}};
        for (int i = 0; i < 4; i++) {
            s8 l = lanes[e][0] < 0 ? -1 : lanes[e][i];
            DW(a + (u32)((index + i * 4) & 15), l < 0 ? 0 : (u8)(g_rsp.v[vt][l] >> 7));
        }
        break;
    }
    case 10: {  // SWV
        u32 a = base + (u32)(off * 16);
        int index = (int)(a & 7);
        a &= ~7u;
        for (int b = e; b < e + 16; b++) DW(a + (u32)(index++ & 15), vb(vt, b & 15));
        break;
    }
    case 11: {  // STV: a transposed store from eight registers
        u32 a = base + (u32)(off * 16);
        int start = vt & ~7, element = 16 - (e & ~1), index = (int)(a & 7) - (e & ~1);
        a &= ~7u;
        for (int r = start; r < start + 8; r++) {
            DW(a + (u32)(index++ & 15), vb(r, element++ & 15));
            DW(a + (u32)(index++ & 15), vb(r, element++ & 15));
        }
        break;
    }
    default: break;
    }
}
