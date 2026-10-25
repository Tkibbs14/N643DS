#pragma once
// NEC VR4300 (MIPS III) interpreter: the exactness reference for everything that runs guest code (docs/DESIGN.md).
#include "../common.h"

enum {
    C0_INDEX = 0, C0_RANDOM = 1, C0_ENTRYLO0 = 2, C0_ENTRYLO1 = 3, C0_CONTEXT = 4, C0_PAGEMASK = 5, C0_WIRED = 6,
    C0_BADVADDR = 8, C0_COUNT = 9, C0_ENTRYHI = 10, C0_COMPARE = 11, C0_STATUS = 12, C0_CAUSE = 13, C0_EPC = 14,
    C0_PRID = 15, C0_CONFIG = 16, C0_LLADDR = 17, C0_WATCHLO = 18, C0_WATCHHI = 19, C0_XCONTEXT = 20,
    C0_PARITYERR = 26, C0_CACHEERR = 27, C0_TAGLO = 28, C0_TAGHI = 29, C0_ERROREPC = 30,
};

// Status bits
#define ST_IE (1u << 0)
#define ST_EXL (1u << 1)
#define ST_ERL (1u << 2)
#define ST_UX (1u << 5)
#define ST_SX (1u << 6)
#define ST_KX (1u << 7)
#define ST_BEV (1u << 22)
#define ST_FR (1u << 26)
#define ST_CU0 (1u << 28)
#define ST_CU1 (1u << 29)

// Exception codes (Cause.ExcCode)
enum {
    EXC_INT = 0, EXC_MOD = 1, EXC_TLBL = 2, EXC_TLBS = 3, EXC_ADEL = 4, EXC_ADES = 5, EXC_IBE = 6, EXC_DBE = 7,
    EXC_SYS = 8, EXC_BP = 9, EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12, EXC_TR = 13, EXC_FPE = 15, EXC_WATCH = 23,
};

typedef struct {
    u64 entryHi, entryLo0, entryLo1;
    u32 pageMask;
} TlbEntry;

typedef struct Vr {
    u64 gpr[32];
    u64 hi, lo;
    u64 pc;      // the instruction being executed
    u64 npc;     // the next one (a branch target once a branch has run)
    u64 cop0[32];
    u64 fpr[32];  // FR=1: 32 64-bit registers; FR=0: fpr[2n] holds the pair n (even = low word, odd = high word)
    u32 fcr31;
    bool llbit;
    TlbEntry tlb[32];

    u64 cycles;      // PClock cycles since power-on; Count = (cycles - countBase) / 2
    u64 countBase;
    u64 compareAt;   // cycles at which Count reaches Compare next

    // Step state
    bool inDelay;      // the current instruction sits in a branch delay slot
    bool nextInDelay;  // set by a branch: the next instruction is its delay slot
    bool excTaken;     // an exception redirected pc / npc during this step
    u64 branchTo;      // npc after the delay slot (set by branches)
    bool branchPending;

    // Instructions retired equal cycles (one cycle per instruction, docs/DESIGN.md "Timing").
} Vr;

extern Vr g_cpu;

void vr_reset(Vr* c);
void vr_run(Vr* c, u64 untilCycles);   // runs until cycles >= untilCycles (or a stop request)
void vr_step(Vr* c);
void vr_set_ip(Vr* c, int bit, bool on);  // Cause.IP bit (2 = RCP via MI, 7 = timer)
u32 vr_count(const Vr* c);
void vr_exception(Vr* c, int code, int ce);
void vr_address_error(Vr* c, u64 va, bool write);
void vr_check_irq(Vr* c);
extern volatile bool g_cpuStop;
extern u64 g_vrRunUntil;
extern bool g_vrIrqPending;
int vr_jit_fallback(Vr* c, u32 op, u32 remaining, u32 inDelay);
extern bool g_jitCodeHit;  // set by jit_code_written: compiled code was overwritten

// Address translation (vr4300_tlb.c). Returns false after raising the exception.
bool vr_translate(Vr* c, u64 vaddr, bool write, u32* paddr);
void vr_tlbr(Vr* c);
void vr_tlbwi(Vr* c);
void vr_tlbwr(Vr* c);
void vr_tlbp(Vr* c);

// FPU (vr4300_fpu.c)
void vr_cop1(Vr* c, u32 op);
u32 vr_cfc1(Vr* c, int rd);
void vr_ctc1(Vr* c, int rd, u32 v);
u64 vr_fpr_read_w(Vr* c, int r);     // MFC1: the 32-bit register r (zero-extended)
void vr_fpr_write_w(Vr* c, int r, u32 v);
u64 vr_fpr_read_d(Vr* c, int r);     // DMFC1 / LDC1 view
void vr_fpr_write_d(Vr* c, int r, u64 v);

// Debugging
void vr_trace_enable(u64 count, u64 fromInstruction);
// Planted bugs that the CPU tests must catch (N64_PLANT=n, core/tests/cpu/run_tests.sh plant): 1 DSUBU, 2 LDR,
// 3 the FPU rounding mode, 4 branch-likely nullification, 5 the SRA quirk, 6 the JIT's ADDU (core/tools/jit_check.sh plant).
extern int g_plant;
extern int g_traceExc;
extern u32 g_watchPa, g_trapPcAbove;
const char* vr_disasm(u32 op, u64 pc, char* buf, size_t n);
