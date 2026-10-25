// The RDP colour combiner as PICA200 TEV stages (cc_pica.h). Each cycle computes (A - B) * C + D for colour and for
// alpha. It is simplified first (C = 0 or A = B: D; B = 0: A * C + D; D = B: interpolate(A, B, C)), and the general
// form becomes subtract(A, B) then multiply-add(previous, C, D), exact while A >= B (the PICA clamps each stage at 0;
// such programs are marked approximate). A stage has one constant colour (separate RGB and alpha parts); a second
// constant goes into the buffer's initial colour, which stages read until something updates the buffer. In the second
// cycle COMBINED is the previous stage's output in the cycle's first stage, and the buffer (the first cycle saves its
// result there) after it. As in the reference renderer (rdp_soft.c), the second cycle's TEXEL0 / TEXEL1 are swapped
// and COMBINED reads as zero in the first cycle.
#include <string.h>
#include "cc_pica.h"

int g_ccPlant;

enum { K_ZERO, K_ONE, K_TEX0, K_TEX1, K_SHADE, K_PRIM, K_ENV, K_COMB, K_PRIMLOD, K_PREV };
typedef struct { u8 kind; bool alpha; } Opd;  // alpha: the source's alpha (in an RGB slot)

static const u8 kSrc6[6] = {K_COMB, K_TEX0, K_TEX1, K_PRIM, K_SHADE, K_ENV};

static Opd rgb_opd(u32 v, int slot, bool* approx) {
    Opd o = {K_ZERO, false};
    if (v < 6) { o.kind = kSrc6[v]; return o; }
    switch (slot) {
    case 0: if (v == 6) o.kind = K_ONE; else if (v == 7) *approx = true; break;  // A: 1, noise
    case 1: if (v == 6 || v == 7) *approx = true; break;                        // B: key centre, K4
    case 2:                                                                       // C
        if (v >= 7 && v <= 12) { o.kind = kSrc6[v - 7]; o.alpha = true; }
        else if (v == 14) o.kind = K_PRIMLOD;
        else if (v == 6 || v == 13 || v == 15) *approx = true;  // key scale, LOD fraction, K5
        break;
    default: if (v == 6) o.kind = K_ONE; break;  // D: 1
    }
    return o;
}

static Opd alpha_opd(u32 v, int slot, bool* approx) {
    Opd o = {K_ZERO, true};
    if (slot == 2) {  // C: LOD fraction, tex0, tex1, prim, shade, env, prim LOD fraction, 0
        if (v >= 1 && v <= 5) o.kind = kSrc6[v];
        else if (v == 6) o.kind = K_PRIMLOD;
        else if (v == 0) *approx = true;
        return o;
    }
    if (v < 6) o.kind = kSrc6[v];
    else if (v == 6) o.kind = K_ONE;
    return o;
}

static bool same(Opd a, Opd b) { return a.kind == b.kind && (a.alpha == b.alpha || a.kind == K_ZERO || a.kind == K_ONE); }

// ---- One channel group (RGB or alpha) of one cycle ----------------------------------------------------------------
typedef struct { u8 src[3], op[3], func; u8 konst; } Micro;  // konst: PK_* or 0xFF (none)
typedef struct {
    Micro m[4];
    int n;
    bool alphaGroup, cycle2, combInBuf;
    bool combLater;  // COMBINED was needed after the cycle's first stage
    int* global;     // this group's part of the buffer's initial colour (PK_*, -1 free)
    bool approx;
} Builder;

static void resolve(Builder* b, Micro* m, int slot, Opd o) {
    u8 op = b->alphaGroup ? (u8)PO_A_ALPHA : (u8)(o.alpha ? PO_ALPHA : PO_COLOR);
    int k = -1;
    switch (o.kind) {
    case K_TEX0: m->src[slot] = PS_TEX0; break;
    case K_TEX1: m->src[slot] = PS_TEX1; break;
    case K_SHADE: m->src[slot] = PS_PRIMARY; break;
    case K_PREV: m->src[slot] = PS_PREVIOUS; break;
    case K_COMB:
        if (b->n == 0) m->src[slot] = PS_PREVIOUS;
        else {
            b->combLater = true;
            if (b->combInBuf) m->src[slot] = PS_BUFFER;
            else { m->src[slot] = PS_PREVIOUS; b->approx = true; }
        }
        break;
    case K_ZERO: k = PK_ZERO; break;
    case K_ONE: k = PK_ONE; break;
    case K_PRIM: k = (o.alpha && !b->alphaGroup) ? PK_PRIM_ALPHA : PK_PRIM; break;
    case K_ENV: k = (o.alpha && !b->alphaGroup) ? PK_ENV_ALPHA : PK_ENV; break;
    default: k = PK_PRIM_LOD; break;
    }
    if (k >= 0) {
        if (!b->alphaGroup) op = PO_COLOR;  // the constant kinds already replicate alphas
        if (m->konst == 0xFF || m->konst == k) { m->konst = (u8)k; m->src[slot] = PS_CONSTANT; }
        else if (!b->combInBuf && (*b->global < 0 || *b->global == k)) { *b->global = k; m->src[slot] = PS_BUFFER; }
        else { m->src[slot] = PS_CONSTANT; b->approx = true; }  // a third constant: wrong value
    }
    m->op[slot] = op;
}

static void emit(Builder* b, u8 func, Opd x, Opd y, Opd z) {
    if (b->n >= 4) { b->approx = true; return; }
    Micro* m = &b->m[b->n];
    memset(m, 0, sizeof *m);
    m->konst = 0xFF;
    m->func = func;
    m->src[0] = m->src[1] = m->src[2] = PS_PREVIOUS;
    m->op[0] = m->op[1] = m->op[2] = b->alphaGroup ? (u8)PO_A_ALPHA : (u8)PO_COLOR;
    resolve(b, m, 0, x);
    if (func != PF_REPLACE) resolve(b, m, 1, y);
    if (func == PF_INTERPOLATE || func == PF_MULTIPLY_ADD || func == PF_ADD_MULTIPLY) resolve(b, m, 2, z);
    b->n++;
}

static void cycle_expr(Builder* b, Opd A, Opd B, Opd C, Opd D) {
    Opd zero = {K_ZERO, false}, prev = {K_PREV, false};
    if (C.kind == K_ZERO || same(A, B)) {  // D
        if (D.kind == K_COMB && b->cycle2) return;  // already the previous stage's output
        emit(b, PF_REPLACE, D, zero, zero);
        return;
    }
    if (B.kind == K_ZERO) {  // A * C + D
        if (D.kind == K_ZERO) {
            if (C.kind == K_ONE) emit(b, PF_REPLACE, A, zero, zero);
            else if (A.kind == K_ONE) emit(b, PF_REPLACE, C, zero, zero);
            else emit(b, g_ccPlant == 1 ? PF_REPLACE : PF_MODULATE, A, C, zero);  // (plant 1: drops C)
        } else if (C.kind == K_ONE) emit(b, PF_ADD, A, D, zero);
        else if (A.kind == K_ONE) emit(b, PF_ADD, C, D, zero);
        else emit(b, PF_MULTIPLY_ADD, A, C, D);
        return;
    }
    if (same(D, B)) {  // A * C + B * (1 - C)
        if (C.kind == K_ONE) emit(b, PF_REPLACE, A, zero, zero);
        else emit(b, PF_INTERPOLATE, A, B, C);
        return;
    }
    // General: max(A - B, 0) * C + D
    b->approx = true;
    emit(b, PF_SUBTRACT, A, B, zero);
    if (D.kind == K_ZERO) {
        if (C.kind != K_ONE) emit(b, PF_MODULATE, prev, C, zero);
    } else if (C.kind == K_ONE) emit(b, PF_ADD, prev, D, zero);
    else emit(b, PF_MULTIPLY_ADD, prev, C, D);
}

static void read_cycle(u64 m, int c, bool alpha, Opd* A, Opd* B, Opd* C, Opd* D, bool* approx) {
    if (!alpha) {
        *A = rgb_opd(c ? (u32)(m >> 37) & 15 : (u32)(m >> 52) & 15, 0, approx);
        *C = rgb_opd(c ? (u32)(m >> 32) & 31 : (u32)(m >> 47) & 31, 2, approx);
        *B = rgb_opd(c ? (u32)(m >> 24) & 15 : (u32)(m >> 28) & 15, 1, approx);
        *D = rgb_opd(c ? (u32)(m >> 6) & 7 : (u32)(m >> 15) & 7, 3, approx);
    } else {
        *A = alpha_opd(c ? (u32)(m >> 21) & 7 : (u32)(m >> 44) & 7, 0, approx);
        *C = alpha_opd(c ? (u32)(m >> 18) & 7 : (u32)(m >> 41) & 7, 2, approx);
        *B = alpha_opd(c ? (u32)(m >> 3) & 7 : (u32)(m >> 12) & 7, 1, approx);
        *D = alpha_opd(c ? (u32)m & 7 : (u32)(m >> 9) & 7, 3, approx);
    }
}

static void build(Builder* b, u64 m, int c, bool cycle2, bool alpha, bool combInBuf, int* global) {
    memset(b, 0, sizeof *b);
    b->alphaGroup = alpha;
    b->cycle2 = cycle2;
    b->combInBuf = combInBuf;
    b->global = global;
    Opd A, B, C, D;
    read_cycle(m, c, alpha, &A, &B, &C, &D, &b->approx);
    Opd* o[4] = {&A, &B, &C, &D};
    for (int k = 0; k < 4; k++) {
        if (cycle2 && o[k]->kind == K_TEX0) o[k]->kind = K_TEX1;
        else if (cycle2 && o[k]->kind == K_TEX1) o[k]->kind = K_TEX0;
        else if (!cycle2 && o[k]->kind == K_COMB) o[k]->kind = K_ZERO;
    }
    cycle_expr(b, A, B, C, D);
}

static Micro pass_micro(bool alpha) {
    Micro m;
    memset(&m, 0, sizeof m);
    m.func = PF_REPLACE;
    m.src[0] = m.src[1] = m.src[2] = PS_PREVIOUS;
    m.op[0] = m.op[1] = m.op[2] = alpha ? (u8)PO_A_ALPHA : (u8)PO_COLOR;
    m.konst = 0xFF;
    return m;
}

void cc_pica_compile(u64 combine, u32 cyc, TevProgram* p) {
    memset(p, 0, sizeof *p);
    int global[2] = {-1, -1};  // RGB, alpha parts of the buffer's initial colour
    Micro out[2][12];
    int n = 0;
    bool bufAfter[12] = {false};
    bool approx = false;
    bool two = cyc == 1;
    for (int ci = 0; ci < (two ? 2 : 1); ci++) {
        int c = two ? ci : 1;  // 1-cycle mode uses the second cycle's fields
        bool cycle2 = two && ci == 1;
        Builder b[2];
        int saved[2] = {global[0], global[1]};
        for (int g = 0; g < 2; g++) build(&b[g], combine, c, cycle2, g == 1, false, &global[g]);
        if (cycle2 && (b[0].combLater || b[1].combLater) && saved[0] < 0 && saved[1] < 0) {
            // COMBINED is needed after the cycle's first stage: the first cycle saves it in the buffer instead
            global[0] = saved[0]; global[1] = saved[1];
            for (int g = 0; g < 2; g++) build(&b[g], combine, c, cycle2, g == 1, true, &global[g]);
            if (n) bufAfter[n - 1] = true;
        }
        int want = b[0].n > b[1].n ? b[0].n : b[1].n;
        for (int k = 0; k < want; k++) {
            if (n >= 12) break;
            for (int g = 0; g < 2; g++) out[g][n] = k < b[g].n ? b[g].m[k] : pass_micro(g == 1);
            n++;
        }
        approx |= b[0].approx | b[1].approx;
    }
    if (n == 0) {  // nothing computed (a combiner of COMBINED alone): black, flagged
        n = 1;
        for (int g = 0; g < 2; g++) { out[g][0] = pass_micro(g == 1); out[g][0].src[0] = PS_CONSTANT; out[g][0].konst = PK_ZERO; }
        approx = true;
    }
    if (n > 6) { n = 6; approx = true; }
    p->n = n;
    for (int i = 0; i < n; i++) {
        TevStage* s = &p->st[i];
        const Micro* r = &out[0][i];
        const Micro* a = &out[1][i];
        for (int k = 0; k < 3; k++) {
            s->srcRgb[k] = r->src[k]; s->opRgb[k] = r->op[k];
            s->srcA[k] = a->src[k]; s->opA[k] = a->op[k];
            if (r->src[k] == PS_TEX0 || a->src[k] == PS_TEX0) p->usesTex0 = true;
            if (r->src[k] == PS_TEX1 || a->src[k] == PS_TEX1) p->usesTex1 = true;
            if (r->src[k] == PS_PRIMARY || a->src[k] == PS_PRIMARY) p->usesShade = true;
        }
        s->funcRgb = r->func; s->funcA = a->func;
        s->constRgb = r->konst == 0xFF ? PK_ZERO : r->konst;
        s->constA = a->konst == 0xFF ? PK_ZERO : a->konst;
        s->bufRgb = s->bufA = bufAfter[i];
    }
    p->globalRgb = global[0] < 0 ? PK_ZERO : global[0];
    p->globalA = global[1] < 0 ? PK_ZERO : global[1];
    p->approx = approx;
}

// ---- Evaluation (the checker's model of the PICA) -----------------------------------------------------------------
static int kval(int k, CcCol prim, CcCol env, int primLod, int ch) {
    int pc = ch == 0 ? prim.r : ch == 1 ? prim.g : ch == 2 ? prim.b : prim.a;
    int ec = ch == 0 ? env.r : ch == 1 ? env.g : ch == 2 ? env.b : env.a;
    switch (k) {
    case PK_ONE: return 255;
    case PK_PRIM: return pc;
    case PK_ENV: return ec;
    case PK_PRIM_ALPHA: return prim.a;
    case PK_ENV_ALPHA: return env.a;
    case PK_PRIM_LOD: return primLod;
    case PK_PRIM_MINUS_ENV: { int v = pc - ec; return v < 0 ? 0 : v; }
    default: return 0;
    }
}

CcCol cc_pica_const(const TevStage* s, CcCol prim, CcCol env, int primLod) {
    CcCol c = {kval(s->constRgb, prim, env, primLod, 0), kval(s->constRgb, prim, env, primLod, 1),
               kval(s->constRgb, prim, env, primLod, 2), kval(s->constA, prim, env, primLod, 3)};
    return c;
}

CcCol cc_pica_buffer(const TevProgram* p, CcCol prim, CcCol env, int primLod) {
    CcCol c = {kval(p->globalRgb, prim, env, primLod, 0), kval(p->globalRgb, prim, env, primLod, 1),
               kval(p->globalRgb, prim, env, primLod, 2), kval(p->globalA, prim, env, primLod, 3)};
    return c;
}

static int chan(CcCol c, int ch) { return ch == 0 ? c.r : ch == 1 ? c.g : ch == 2 ? c.b : c.a; }
static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static int func_eval(u8 f, int a, int b, int c) {
    switch (f) {
    case PF_REPLACE: return a;
    case PF_MODULATE: return a * b / 255;
    case PF_ADD: return clamp255(a + b);
    case PF_ADD_SIGNED: return clamp255(a + b - 128);
    case PF_INTERPOLATE: return clamp255((a * c + b * (255 - c)) / 255);
    case PF_SUBTRACT: return clamp255(a - b);
    case PF_MULTIPLY_ADD: return clamp255(a * b / 255 + c);
    case PF_ADD_MULTIPLY: return clamp255(a + b) * c / 255;
    default: return 0;
    }
}

CcCol cc_pica_eval(const TevProgram* p, CcCol tex0, CcCol tex1, CcCol shade, CcCol prim, CcCol env, int primLod) {
    CcCol prev = {0, 0, 0, 0};
    CcCol buf = cc_pica_buffer(p, prim, env, primLod), next = buf;
    for (int i = 0; i < p->n; i++) {
        const TevStage* s = &p->st[i];
        CcCol k = cc_pica_const(s, prim, env, primLod);
        int v[4];
        for (int ch = 0; ch < 4; ch++) {
            bool a = ch == 3;
            int x[3];
            for (int j = 0; j < 3; j++) {
                u8 src = a ? s->srcA[j] : s->srcRgb[j], op = a ? s->opA[j] : s->opRgb[j];
                CcCol c = src == PS_TEX0 ? tex0 : src == PS_TEX1 ? tex1 : src == PS_PRIMARY ? shade : src == PS_CONSTANT ? k
                        : src == PS_BUFFER ? buf : prev;
                if (a) x[j] = op == PO_A_ONE_MINUS_ALPHA ? 255 - c.a : c.a;
                else x[j] = op == PO_COLOR ? chan(c, ch) : op == PO_ONE_MINUS_COLOR ? 255 - chan(c, ch) : op == PO_ALPHA ? c.a : 255 - c.a;
            }
            v[ch] = func_eval(a ? s->funcA : s->funcRgb, x[0], x[1], x[2]);
        }
        CcCol out = {v[0], v[1], v[2], v[3]};
        // The buffer lags: a stage's output written to it is read from two stages on (Citra's TEV loop: the stage
        // after reads the buffer as it was before)
        buf = next;
        if (s->bufRgb) { next.r = out.r; next.g = out.g; next.b = out.b; }
        if (s->bufA) next.a = out.a;
        prev = out;
    }
    return prev;
}
