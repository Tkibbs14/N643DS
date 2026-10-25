// Exceptions (cartridge only): overflow, traps, SYSCALL / BREAK, address errors (data and fetch), reserved
// instruction, coprocessor unusable, a fault in a delay slot, interrupts (software and Count / Compare), the TLB
// (hit, invalid, refill, modified, TLBP, TLBR), floating-point traps and LL / SC across ERET. Expected values are the
// VR4300 manual's (chapter 6); the hardware-derived suites (docs/TEST_ROMS.md) are the stronger reference.
#include "exc.h"

static u32 g_buf[4] __attribute__((aligned(8))) = {0x11223344, 0x55667788, 0x99AABBCC, 0xDDEEFF00};
#define SX(p) ((u64)(s64)(s32)(u32)(p))

static void clear(void) {
    g_exc.count = 0; g_exc.cause = 0; g_exc.epc = 0; g_exc.badvaddr = 0; g_exc.resume = 0; g_exc.vector = 0xFFFF;
}

#define EXC(what, code, vec) do { \
    EXPECT(what " taken once", g_exc.count, 1); \
    EXPECT(what " code", EXCCODE(g_exc.cause), code); \
    EXPECT(what " vector", g_exc.vector, vec); } while (0)

static void arith(void) {
    u64 r, at;
    clear();
    __asm__ volatile(".set noreorder\n\tla %1, 1f\n\tli %0, 0x1234\n1:\tadd %0, %2, %3\n\t.set reorder"
                     : "=&r"(r), "=&r"(at) : "r"(0x7FFFFFFFull), "r"(1ull));
    EXC("add overflow", 12, 0x180);
    EXPECT("add overflow leaves rd", r, 0x1234);
    EXPECT("add overflow epc", g_exc.epc, at);
    clear();
    __asm__ volatile("li %0, 0x77\n\tdadd %0, %1, %2" : "=&r"(r) : "r"(0x7FFFFFFFFFFFFFFFull), "r"(1ull));
    EXC("dadd overflow", 12, 0x180);
    EXPECT("dadd overflow leaves rd", r, 0x77);
    clear();
    __asm__ volatile("li %0, 0x55\n\taddi %0, %1, -1" : "=&r"(r) : "r"(0xFFFFFFFF80000000ull));
    EXC("addi overflow", 12, 0x180);
    clear();
    __asm__ volatile("teq $zero, $zero");
    EXC("teq", 13, 0x180);
    clear();
    __asm__ volatile("tnei $zero, 0");
    EXPECT("tnei not taken", g_exc.count, 0);
    clear();
    __asm__ volatile("syscall");
    EXC("syscall", 8, 0x180);
    clear();
    __asm__ volatile("break");
    EXC("break", 9, 0x180);
    clear();
    __asm__ volatile(".word 0x70000000");  // primary opcode 0x1C: reserved on the VR4300
    EXC("reserved instruction", 10, 0x180);
}

extern char fetch_resume[], bd_branch[], bd_resume[];

static void address(void) {
    u64 r;
    const u8* p = (const u8*)g_buf;
    clear();
    __asm__ volatile("lw %0, 0(%1)" : "=r"(r) : "r"(p + 1) : "memory");
    EXC("lw unaligned", 4, 0x180);
    EXPECT("lw unaligned badvaddr", g_exc.badvaddr, SX(p + 1));
    clear();
    __asm__ volatile("sw %0, 0(%1)" : : "r"(0ull), "r"(p + 2) : "memory");
    EXC("sw unaligned", 5, 0x180);
    EXPECT("sw unaligned badvaddr", g_exc.badvaddr, SX(p + 2));
    EXPECT("sw unaligned stored nothing", g_buf[0], 0x11223344);
    clear();
    __asm__ volatile("ld %0, 0(%1)" : "=r"(r) : "r"(p + 4) : "memory");
    EXC("ld at 4 mod 8", 4, 0x180);
    clear();
    __asm__ volatile("lh %0, 0(%1)" : "=r"(r) : "r"(p + 3) : "memory");
    EXC("lh odd", 4, 0x180);
    // Fetch from a misaligned address: EPC and BadVAddr are the target
    clear();
    g_exc.resume = SX(fetch_resume);
    __asm__ volatile(".set noreorder\n\tla $t0, fetch_resume\n\tori $t0, $t0, 2\n\tjr $t0\n\tnop\n"
                     ".globl fetch_resume\nfetch_resume:\tnop\n\t.set reorder" : : : "t0");
    EXC("fetch misaligned", 4, 0x180);
    EXPECT("fetch misaligned epc", g_exc.epc, SX(fetch_resume) | 2);
    EXPECT("fetch misaligned badvaddr", g_exc.badvaddr, SX(fetch_resume) | 2);
    // A fault in a delay slot: EPC is the branch, Cause.BD set
    clear();
    g_exc.resume = SX(bd_resume);
    __asm__ volatile(".set noreorder\n.globl bd_branch\nbd_branch:\tbeq $zero, $zero, bd_resume\n\t"
                     "lw $t0, 1(%0)\n\tnop\n.globl bd_resume\nbd_resume:\tnop\n\t.set reorder" : : "r"(p) : "t0");
    EXC("delay slot fault", 4, 0x180);
    EXPECT("delay slot epc is the branch", g_exc.epc, SX(bd_branch));
    EXPECT("delay slot BD", g_exc.cause >> 31, 1);
}

static void coprocessor(void) {
    u32 st = c0_status();
    clear();
    c0_set_status(st & ~(1u << 29));  // CU1 off
    u32 r;
    __asm__ volatile("mfc1 %0, $f0" : "=r"(r));
    c0_set_status(st);
    EXC("cop1 unusable", 11, 0x180);
    EXPECT("cop1 unusable CE", (g_exc.cause >> 28) & 3, 1);
}

static void interrupts(void) {
    u32 st = c0_status();
    // Software interrupt IP0
    clear();
    c0_set_status(st | 1u | (1u << 8));
    __asm__ volatile("mfc0 $t0, $13\n\tori $t0, $t0, 0x100\n\tmtc0 $t0, $13\n\tnop\n\tnop" : : : "t0");
    c0_set_status(st);
    EXC("software interrupt", 0, 0x180);
    EXPECT("software interrupt IP0", (g_exc.cause >> 8) & 1, 1);
    // Count / Compare
    clear();
    u32 cmp = c0_count() + 200;
    __asm__ volatile("mtc0 %0, $11" : : "r"(cmp));
    c0_set_status(st | 1u | (1u << 15));
    for (volatile int i = 0; i < 100000 && !g_exc.count; i++) {}
    c0_set_status(st);
    EXC("timer interrupt", 0, 0x180);
    EXPECT("timer interrupt IP7", (g_exc.cause >> 15) & 1, 1);
}

#define MTC0(r, v) __asm__ volatile("dmtc0 %0, $" #r "\n\tnop" : : "r"((u64)(v)))
#define MFC0(r) ({ u64 _v; __asm__ volatile("dmfc0 %0, $" #r : "=r"(_v)); _v; })

static void tlb_entry(int index, u32 vaddr, u32 lo0, u32 lo1) {
    MTC0(5, 0);                 // PageMask: 4 KB pages
    MTC0(10, vaddr & ~0x1FFFu); // EntryHi: VPN2, ASID 0
    MTC0(2, lo0);
    MTC0(3, lo1);
    MTC0(0, index);
    __asm__ volatile("tlbwi\n\tnop");
}
#define LO(pa, d, v, g) ((((pa) >> 12) << 6) | (3u << 3) | ((d) << 2) | ((v) << 1) | (g))

static void tlb(void) {
    volatile u32* phys = (volatile u32*)0xA0200000u;
    phys[0] = 0xCAFEF00D;
    phys[1] = 0;
    tlb_entry(5, 0x00010000, LO(0x00200000, 1, 1, 1), LO(0x00300000, 1, 0, 1));
    tlb_entry(6, 0x00020000, LO(0x00201000, 0, 1, 1), LO(0x00301000, 0, 0, 1));
    clear();
    u32 v = *(volatile u32*)0x00010000u;
    EXPECT("tlb hit: no exception", g_exc.count, 0);
    EXPECT("tlb hit: data", v, 0xCAFEF00D);
    *(volatile u32*)0x00010004u = 0x12345678;
    EXPECT("tlb store through", phys[1], 0x12345678);
    clear();
    v = *(volatile u32*)0x00011000u;  // odd page of entry 5: not valid
    EXC("tlb invalid load", 2, 0x180);
    EXPECT("tlb invalid badvaddr", g_exc.badvaddr, 0x11000);
    clear();
    v = *(volatile u32*)0x00400000u;  // no entry
    EXC("tlb refill load", 2, 0x000);
    EXPECT("tlb refill entryhi", MFC0(10) & ~0xFFull, 0x00400000);
    EXPECT("tlb refill context", (MFC0(4) >> 4) & 0x7FFFF, 0x00400000 >> 13);
    clear();
    *(volatile u32*)0x00400000u = 1;
    EXC("tlb refill store", 3, 0x000);
    clear();
    *(volatile u32*)0x00020000u = 1;  // D clear
    EXC("tlb modified", 1, 0x180);
    clear();
    v = *(volatile u32*)0x00020000u;
    EXPECT("tlb read-only page loads", g_exc.count, 0);
    MTC0(10, 0x00010000);
    __asm__ volatile("tlbp\n\tnop");
    EXPECT("tlbp hit", MFC0(0), 5);
    MTC0(10, 0x00400000);
    __asm__ volatile("tlbp\n\tnop");
    EXPECT("tlbp miss sets P", MFC0(0) >> 31, 1);
    MTC0(0, 5);
    __asm__ volatile("tlbr\n\tnop");
    EXPECT("tlbr entrylo0", MFC0(2), LO(0x00200000, 1, 1, 1));
    EXPECT("tlbr entryhi", MFC0(10), 0x00010000);
    (void)v;
}

static void fpe(void) {
    u32 r, fcr;
    __asm__ volatile("ctc1 $zero, $31");
    clear();
    __asm__ volatile("mtc1 %1, $f0\n\tadd.s $f2, $f0, $f0\n\tmfc1 %0, $f2" : "=r"(r) : "r"(1u) : "$f0", "$f2");
    __asm__ volatile("cfc1 %0, $31" : "=r"(fcr));
    __asm__ volatile("ctc1 $zero, $31");
    EXC("denormal operand", 15, 0x180);
    EXPECT("denormal operand: cause E", (fcr >> 17) & 1, 1);
    clear();
    __asm__ volatile("ctc1 %0, $31" : : "r"(1u << 11));  // enable V
    __asm__ volatile("mtc1 %1, $f0\n\tsqrt.s $f2, $f0\n\tmfc1 %0, $f2" : "=r"(r) : "r"(0xBF800000u) : "$f0", "$f2");
    __asm__ volatile("cfc1 %0, $31" : "=r"(fcr));
    __asm__ volatile("ctc1 $zero, $31");
    EXC("sqrt(-1) with V enabled", 15, 0x180);
    EXPECT("sqrt(-1): cause V", (fcr >> 16) & 1, 1);
    clear();
    __asm__ volatile("mtc1 %1, $f0\n\tcvt.w.s $f2, $f0\n\tmfc1 %0, $f2" : "=r"(r) : "r"(0x4F32D05Eu) : "$f0", "$f2");
    __asm__ volatile("ctc1 $zero, $31");
    EXC("cvt.w.s out of range (unimplemented)", 15, 0x180);
    clear();
    __asm__ volatile("mtc1 %1, $f0\n\tcvt.w.s $f2, $f0\n\tmfc1 %0, $f2" : "=r"(r) : "r"(0x7FBFFFFFu) : "$f0", "$f2");
    __asm__ volatile("ctc1 $zero, $31");
    EXC("cvt.w.s NaN (unimplemented)", 15, 0x180);
    clear();
    __asm__ volatile("mtc1 %1, $f0\n\tmul.s $f2, $f0, $f0\n\tmfc1 %0, $f2" : "=r"(r) : "r"(0x7E967699u) : "$f0", "$f2");
    __asm__ volatile("cfc1 %0, $31" : "=r"(fcr));
    __asm__ volatile("ctc1 $zero, $31");
    EXPECT("overflow untrapped: no exception", g_exc.count, 0);
    EXPECT("overflow untrapped: infinity", r, 0x7F800000);
    EXPECT("overflow untrapped: cause O I", (fcr >> 12) & 63, 5);
}

static void llsc(void) {
    u64 ok;
    clear();
    __asm__ volatile("ll $t0, 0(%1)\n\tsyscall\n\tli %0, 7\n\tsc %0, 0(%1)" : "=&r"(ok) : "r"(g_buf) : "t0", "memory");
    EXPECT("sc after eret fails", ok, 0);
    __asm__ volatile("ll $t0, 0(%1)\n\tli %0, 7\n\tsc %0, 0(%1)" : "=&r"(ok) : "r"(g_buf) : "t0", "memory");
    EXPECT("sc without eret succeeds", ok, 1);
}

int test_main(void) {
    exc_install();
    arith();
    address();
    coprocessor();
    interrupts();
    tlb();
    fpe();
    llsc();
    return 0;
}
