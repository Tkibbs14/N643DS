// COP0 and COP1 state (cartridge only): identification, write masks, Random and Wired, the COP0 latch, Count, and
// the FR=0 register pairs.
#include "exc.h"

#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r "\n\tnop" : : "r"((u32)(v)))
#define DMTC0(r, v) __asm__ volatile("dmtc0 %0, $" #r "\n\tnop" : : "r"((u64)(v)))
#define MFC0(r) ({ u64 _v; __asm__ volatile("mfc0 %0, $" #r : "=r"(_v)); _v; })
#define DMFC0(r) ({ u64 _v; __asm__ volatile("dmfc0 %0, $" #r : "=r"(_v)); _v; })

int test_main(void) {
    exc_install();
    EXPECT("PRId", MFC0(15), 0x00000B22);
    // Random counts between Wired and 31; writing Wired sets it to 31
    for (int i = 0; i < 50; i++) {
        u32 r = (u32)MFC0(1);
        if (r > 31) EXPECT("Random <= 31", r, 31);
    }
    MTC0(6, 10);
    EXPECT("Wired", MFC0(6), 10);
    for (int i = 0; i < 50; i++) {
        u32 r = (u32)MFC0(1);
        if (r < 10 || r > 31) EXPECT("Random in [Wired, 31]", r, 20);
    }
    MTC0(6, 0);
    // Cause: only IP0 / IP1 are writable
    MTC0(13, 0xFFFFFFFF);
    EXPECT("Cause writable bits", MFC0(13) & 0x300, 0x300);
    MTC0(13, 0);
    // Index keeps 6 bits; PageMask bits 24:13; EntryLo 30 bits
    MTC0(0, 0xFFFFFFFF);
    EXPECT("Index mask", MFC0(0) & 0x7FFFFFFF, 0x3F);
    MTC0(5, 0xFFFFFFFF);
    EXPECT("PageMask mask", MFC0(5), 0x01FFE000);
    MTC0(5, 0);
    MTC0(2, 0xFFFFFFFF);
    EXPECT("EntryLo0 mask", MFC0(2), 0x3FFFFFFF);
    // The latch: unimplemented COP0 registers read back the last value written to any COP0 register (ares)
    MTC0(2, 0x12345678);
    EXPECT("cop0 latch (reg 7)", MFC0(7), 0x12345678);
    DMTC0(14, 0x0123456789ABCDEFull);  // EPC is 64-bit
    EXPECT("EPC 64-bit", DMFC0(14), 0x0123456789ABCDEFull);
    EXPECT("cop0 latch 64-bit (reg 21)", DMFC0(21), 0x0123456789ABCDEFull);
    // Count advances
    u32 c0 = c0_count();
    for (volatile int i = 0; i < 1000; i++) {}
    u32 c1 = c0_count();
    EXPECT("Count advances", c1 != c0, 1);
    MTC0(9, 0x1000);
    u32 c2 = c0_count();
    EXPECT("Count written", c2 >= 0x1000 && c2 < 0x1100, 1);
    // FR=0: an odd FPR is the high word of the even one (MIPS III)
    u32 st = c0_status();
    c0_set_status(st & ~(1u << 26));
    u64 pair;
    __asm__ volatile("mtc1 %1, $f0\n\tmtc1 %2, $f1\n\tdmfc1 %0, $f0" : "=r"(pair) : "r"(0x89ABCDEFu), "r"(0x01234567u) : "$f0", "$f1");
    u32 odd;
    __asm__ volatile("dmtc1 %1, $f2\n\tmfc1 %0, $f3" : "=r"(odd) : "r"(0xFEDCBA9876543210ull) : "$f2", "$f3");
    c0_set_status(st);
    EXPECT("FR=0 pair", pair, 0x0123456789ABCDEFull);
    EXPECT("FR=0 odd register", odd, 0xFEDCBA98);
    // FCR0
    u32 fcr0;
    __asm__ volatile("cfc1 %0, $0" : "=r"(fcr0));
    EXPECT("FCR0", fcr0, 0x00000A00);
    EXPECT("no stray exceptions", g_exc.count, 0);
    return 0;
}
