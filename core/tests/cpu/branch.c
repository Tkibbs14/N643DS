// Branches and jumps: taken / not taken, delay slots run, likely branches nullify their slot when not taken, links
// point past the delay slot. Each case returns a bit mask of the instructions that ran.
#include "plat.h"

// t0 accumulates bits: 1 = the delay slot ran, 2 = the fall-through path ran, 4 = the target ran.
#define BR2(fn, insn) \
    static u64 fn(u64 a, u64 b) { \
        u64 r; \
        __asm__ volatile(".set noreorder\n\t" \
                         "move %0, $zero\n\t" \
                         insn " %1, %2, 1f\n\t" \
                         "ori %0, %0, 1\n\t" \
                         "ori %0, %0, 2\n\t" \
                         "1: ori %0, %0, 4\n\t" \
                         ".set reorder" \
                         : "=&r"(r) : "r"(a), "r"(b)); \
        return r; \
    }
#define BR1(fn, insn) \
    static u64 fn(u64 a) { \
        u64 r; \
        __asm__ volatile(".set noreorder\n\t" \
                         "move %0, $zero\n\t" \
                         insn " %1, 1f\n\t" \
                         "ori %0, %0, 1\n\t" \
                         "ori %0, %0, 2\n\t" \
                         "1: ori %0, %0, 4\n\t" \
                         ".set reorder" \
                         : "=&r"(r) : "r"(a)); \
        return r; \
    }
BR2(b_beq, "beq") BR2(b_bne, "bne") BR2(b_beql, "beql") BR2(b_bnel, "bnel")
BR1(b_blez, "blez") BR1(b_bgtz, "bgtz") BR1(b_bltz, "bltz") BR1(b_bgez, "bgez")
BR1(b_blezl, "blezl") BR1(b_bgtzl, "bgtzl") BR1(b_bltzl, "bltzl") BR1(b_bgezl, "bgezl")

// The and-link forms: also report ra relative to the instruction after the delay slot (0 when correct).
#define BAL(fn, insn) \
    static u64 fn(u64 a, u64* link) { \
        u64 r, ra, ref; \
        __asm__ volatile(".set noreorder\n\t" \
                         "move %0, $zero\n\t" \
                         insn " %3, 1f\n\t" \
                         "ori %0, %0, 1\n\t" \
                         "2: ori %0, %0, 2\n\t" \
                         "1: ori %0, %0, 4\n\t" \
                         "move %1, $ra\n\t" \
                         "la %2, 2b\n\t" \
                         ".set reorder" \
                         : "=&r"(r), "=&r"(ra), "=&r"(ref) : "r"(a) : "ra"); \
        *link = ra - ref; \
        return r; \
    }
BAL(b_bltzal, "bltzal") BAL(b_bgezal, "bgezal") BAL(b_bltzall, "bltzall") BAL(b_bgezall, "bgezall")

static u64 jumps(void) {
    u64 r, d1, d2;
    __asm__ volatile(".set noreorder\n\t"
                     "move %0, $zero\n\t"
                     "j 1f\n\t"
                     "ori %0, %0, 1\n\t"
                     "ori %0, %0, 2\n\t"
                     "1: la $t0, 3f\n\t"
                     "jalr $t1, $t0\n\t"
                     "ori %0, %0, 8\n\t"
                     "4: ori %0, %0, 16\n\t"
                     "3: la $t2, 4b\n\t"
                     "dsubu %1, $t1, $t2\n\t"
                     "jal 5f\n\t"
                     "ori %0, %0, 32\n\t"
                     "6: ori %0, %0, 64\n\t"
                     "5: la $t2, 6b\n\t"
                     "dsubu %2, $ra, $t2\n\t"
                     ".set reorder"
                     : "=&r"(r), "=&r"(d1), "=&r"(d2) : : "t0", "t1", "t2", "ra");
    return r | (d1 << 16) | (d2 << 32);
}

int test_main(void) {
    rnd_seed(3);
    static const u64 vals[] = {0, 1, ~0ull, 0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull, 0x80000000ull, 5};
    const int n = sizeof vals / sizeof vals[0];
    out_begin("beq/bne/beql/bnel");
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            out(b_beq(vals[i], vals[j])); out(b_bne(vals[i], vals[j]));
            out(b_beql(vals[i], vals[j])); out(b_bnel(vals[i], vals[j]));
        }
    out_end();
    out_begin("compare-with-zero");
    for (int i = 0; i < n; i++) {
        u64 v = vals[i];
        out(b_blez(v)); out(b_bgtz(v)); out(b_bltz(v)); out(b_bgez(v));
        out(b_blezl(v)); out(b_bgtzl(v)); out(b_bltzl(v)); out(b_bgezl(v));
    }
    out_end();
    out_begin("and-link");
    for (int i = 0; i < n; i++) {
        u64 link;
        out(b_bltzal(vals[i], &link)); out(link);
        out(b_bgezal(vals[i], &link)); out(link);
        out(b_bltzall(vals[i], &link)); out(link);
        out(b_bgezall(vals[i], &link)); out(link);
    }
    out_end();
    out_begin("jumps");
    out(jumps());
    out_end();
    // The same facts as stated values, so a wrong qemu could not hide a wrong emulator
    EXPECT("beq taken", b_beq(1, 1), 5);
    EXPECT("beq not taken", b_beq(1, 2), 7);
    EXPECT("beql not taken nullifies", b_beql(1, 2), 6);
    EXPECT("bnel taken", b_bnel(1, 2), 5);
    u64 link;
    EXPECT("bltzal not taken", b_bltzal(1, &link), 7);
    EXPECT("bltzal links anyway", link, 0);
    EXPECT("bltzall not taken nullifies", b_bltzall(1, &link), 6);
    EXPECT("jumps (slots run, skipped code not, links exact)", jumps(), 1 | 8 | 32);
    return 0;
}
