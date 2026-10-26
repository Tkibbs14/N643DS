// The PICA combiner's checker (n64emu --cc-check): every (combine word, cycle type) a run drew with goes through
// cc_pica_compile, and random inputs through both cc_pica_eval (the PICA's arithmetic) and the reference combiner
// (rdp_soft.c). Programs the compiler calls exact must agree within rounding (3 per channel: the PICA divides by 255,
// the reference by 256); approximate ones are counted and their worst case reported. N64_CC_PLANT=1 makes every
// A * C stage drop C (the check must then report differences, or it proves nothing).
#include <stdio.h>
#include <stdlib.h>
#include "../sys/sys.h"
#include "rdp.h"
#include "cc_pica.h"

void rdp_combine_ref(const int t0[4], const int t1[4], const int shade[4], int out[4]);
extern u64 g_rdpModes[512][2];
extern int g_rdpModeN;
extern bool g_decodeCheckOn;
extern int g_decodePlant;
extern u64 g_decodeChecked, g_decodeDiffer;

static u32 g_seed = 12345;
static int rnd(void) { g_seed = g_seed * 1103515245u + 12345u; return (int)((g_seed >> 16) & 255); }

int cc_check(void) {
    const char* plant = getenv("N64_CC_PLANT");
    g_ccPlant = plant ? atoi(plant) : 0;
    int exact = 0, exactBad = 0, approx = 0;
    RdpState saved = g_rdp;
    for (int i = 0; i < g_rdpModeN; i++) {
        u64 combine = g_rdpModes[i][0];
        u32 cyc = (u32)g_rdpModes[i][1];
        TevProgram p;
        cc_pica_compile(combine, cyc, &p);
        g_rdp.combine = combine;
        g_rdp.om = (g_rdp.om & ~(3ull << 52)) | ((u64)cyc << 52);
        int worst = 0;
        for (int k = 0; k < 2000; k++) {
            int t0[4], t1[4], sh[4], ref[4];
            for (int c = 0; c < 4; c++) { t0[c] = rnd(); t1[c] = rnd(); sh[c] = rnd(); }
            CcCol prim = {rnd(), rnd(), rnd(), rnd()}, env = {rnd(), rnd(), rnd(), rnd()};
            int lod = rnd();
            g_rdp.prim = (u32)prim.r << 24 | (u32)prim.g << 16 | (u32)prim.b << 8 | (u32)prim.a;
            g_rdp.env = (u32)env.r << 24 | (u32)env.g << 16 | (u32)env.b << 8 | (u32)env.a;
            g_rdp.primLod = (u32)lod;
            rdp_combine_ref(t0, t1, sh, ref);
            CcCol a = {t0[0], t0[1], t0[2], t0[3]}, b = {t1[0], t1[1], t1[2], t1[3]}, s = {sh[0], sh[1], sh[2], sh[3]};
            CcCol o = cc_pica_eval(&p, a, b, s, prim, env, lod);
            int got[4] = {o.r, o.g, o.b, o.a};
            for (int c = 0; c < 4; c++) {
                int d = abs(got[c] - ref[c]);
                if (d > worst) worst = d;
            }
        }
        if (p.approx) approx++;
        else if (worst <= 3) exact++;
        else exactBad++;
        if ((!p.approx && worst > 3) || getenv("N64_CC_VERBOSE"))
            fprintf(stderr, "cc %016llx cycle %u: %d stages, %s, worst difference %d\n", (unsigned long long)combine,
                    (unsigned)cyc + 1, p.n, p.approx ? "approximate" : "exact", worst);
    }
    g_rdp = saved;
    fprintf(stderr, "cc check: %d modes: %d exact and agreeing, %d exact but differing, %d approximate%s\n", g_rdpModeN,
            exact, exactBad, approx, g_ccPlant ? " (planted bug)" : "");
    fprintf(stderr, "texture decode check: %llu texels, %llu differ%s\n", (unsigned long long)g_decodeChecked,
            (unsigned long long)g_decodeDiffer, g_decodePlant ? " (planted bug)" : "");
    return exactBad + (g_decodeDiffer != 0);
}
