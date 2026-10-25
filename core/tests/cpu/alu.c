// Integer unit: every ALU, shift, multiply and divide instruction over generated operands. 32-bit instructions get
// sign-extended operands in the qemu comparison (other inputs are UNPREDICTABLE in MIPS III; the VR4300's answer for
// them is an EXPECT). Overflowing ADD / SUB and division by zero are EXPECTs too.
#include "plat.h"

#define N 2000

#define R3(fn, insn) \
    static u64 fn(u64 a, u64 b) { u64 r; __asm__ volatile(insn " %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); return r; }
R3(op_daddu, "daddu") R3(op_dsubu, "dsubu") R3(op_and, "and") R3(op_or, "or") R3(op_xor, "xor") R3(op_nor, "nor")
R3(op_slt, "slt") R3(op_sltu, "sltu") R3(op_addu, "addu") R3(op_subu, "subu") R3(op_add, "add") R3(op_sub, "sub")
R3(op_dadd, "dadd") R3(op_dsub, "dsub")
// variable shifts: rd, rt, rs
#define RV(fn, insn) \
    static u64 fn(u64 a, u64 s) { u64 r; __asm__ volatile(insn " %0, %1, %2" : "=r"(r) : "r"(a), "r"(s)); return r; }
RV(op_sllv, "sllv") RV(op_srlv, "srlv") RV(op_srav, "srav") RV(op_dsllv, "dsllv") RV(op_dsrlv, "dsrlv")
RV(op_dsrav, "dsrav")

// Divides name $zero as the destination so the assembler emits the bare instruction (no zero-divisor checks).
#define MD(fn, insn) \
    static void fn(u64 a, u64 b, u64* hi, u64* lo) { \
        u64 h, l; \
        __asm__ volatile(insn " %2, %3\n\tmfhi %0\n\tmflo %1" : "=r"(h), "=r"(l) : "r"(a), "r"(b) : "hi", "lo"); \
        *hi = h; *lo = l; \
    }
MD(op_mult, "mult") MD(op_multu, "multu") MD(op_div, "div $zero,") MD(op_divu, "divu $zero,") MD(op_dmult, "dmult")
MD(op_dmultu, "dmultu") MD(op_ddiv, "ddiv $zero,") MD(op_ddivu, "ddivu $zero,")

// Immediate forms: a fixed set of immediates, one function each.
#define IMMS(X) X(0) X(1) X(-1) X(0x7FFF) X(-0x8000) X(0x1234)
#define UIMMS(X) X(0) X(1) X(0xFFFF) X(0x8000) X(0x1234) X(0x7FFF)

static void run3(const char* name, u64 (*fn)(u64, u64), int sx) {
    out_begin(name);
    for (int i = 0; i < N; i++) {
        u64 a = sx ? val32() : val64(), b = sx ? val32() : val64();
        out(fn(a, b));
    }
    out_end();
}

static int ovf32(u64 a, u64 b, int sub) {
    s64 r = sub ? (s64)(s32)a - (s64)(s32)b : (s64)(s32)a + (s64)(s32)b;
    return r != (s64)(s32)r;
}

static void run_trapping(void) {
    out_begin("add");
    for (int i = 0; i < N; i++) { u64 a = val32(), b = val32(); if (!ovf32(a, b, 0)) out(op_add(a, b)); }
    out_end();
    out_begin("sub");
    for (int i = 0; i < N; i++) { u64 a = val32(), b = val32(); if (!ovf32(a, b, 1)) out(op_sub(a, b)); }
    out_end();
    out_begin("dadd");
    for (int i = 0; i < N; i++) {
        u64 a = val64(), b = val64(), r = a + b;
        if (!((s64)(~(a ^ b) & (a ^ r)) < 0)) out(op_dadd(a, b));
    }
    out_end();
    out_begin("dsub");
    for (int i = 0; i < N; i++) {
        u64 a = val64(), b = val64(), r = a - b;
        if (!((s64)((a ^ b) & (a ^ r)) < 0)) out(op_dsub(a, b));
    }
    out_end();
}

static void run_varshift(void) {
    out_begin("sllv"); for (int i = 0; i < N; i++) out(op_sllv(val32(), rnd())); out_end();
    out_begin("srlv"); for (int i = 0; i < N; i++) out(op_srlv(val32(), rnd())); out_end();
    out_begin("srav"); for (int i = 0; i < N; i++) out(op_srav(val32(), rnd())); out_end();
    out_begin("dsllv"); for (int i = 0; i < N; i++) out(op_dsllv(val64(), rnd())); out_end();
    out_begin("dsrlv"); for (int i = 0; i < N; i++) out(op_dsrlv(val64(), rnd())); out_end();
    out_begin("dsrav"); for (int i = 0; i < N; i++) out(op_dsrav(val64(), rnd())); out_end();
}

// Shifts by an immediate: every shift amount.
#define SH_CASES(insn) \
    S(insn,0) S(insn,1) S(insn,2) S(insn,3) S(insn,4) S(insn,5) S(insn,6) S(insn,7) S(insn,8) S(insn,9) S(insn,10) \
    S(insn,11) S(insn,12) S(insn,13) S(insn,14) S(insn,15) S(insn,16) S(insn,17) S(insn,18) S(insn,19) S(insn,20) \
    S(insn,21) S(insn,22) S(insn,23) S(insn,24) S(insn,25) S(insn,26) S(insn,27) S(insn,28) S(insn,29) S(insn,30) S(insn,31)
#define S(insn, n) case n: __asm__ volatile(#insn " %0, %1, " #n : "=r"(r) : "r"(a)); break;
#define SHF(insn) static u64 sh_##insn(u64 a, int n) { u64 r = 0; switch (n) { SH_CASES(insn) } return r; }
SHF(sll) SHF(srl) SHF(sra) SHF(dsll) SHF(dsrl) SHF(dsra) SHF(dsll32) SHF(dsrl32) SHF(dsra32)
#undef S

static void run_shift(const char* name, u64 (*fn)(u64, int), int sx) {
    out_begin(name);
    for (int n = 0; n < 32; n++)
        for (int i = 0; i < 40; i++) out(fn(sx ? val32() : val64(), n));
    out_end();
}

// Immediate instructions, one asm per immediate
#define IFN(insn, list) static u64 im_##insn(u64 a, int k) { u64 r = 0; int j = 0; list(I_##insn) (void)j; return r; }
#define DO_I(insn, imm) if (k == j++) __asm__ volatile(#insn " %0, %1, " #imm : "=r"(r) : "r"(a));
#define I_addiu(imm) DO_I(addiu, imm)
#define I_daddiu(imm) DO_I(daddiu, imm)
#define I_slti(imm) DO_I(slti, imm)
#define I_sltiu(imm) DO_I(sltiu, imm)
#define I_andi(imm) DO_I(andi, imm)
#define I_ori(imm) DO_I(ori, imm)
#define I_xori(imm) DO_I(xori, imm)
IFN(addiu, IMMS) IFN(daddiu, IMMS) IFN(slti, IMMS) IFN(sltiu, IMMS) IFN(andi, UIMMS) IFN(ori, UIMMS) IFN(xori, UIMMS)

static void run_imm(const char* name, u64 (*fn)(u64, int), int sx) {
    out_begin(name);
    for (int k = 0; k < 6; k++)
        for (int i = 0; i < 200; i++) out(fn(sx ? val32() : val64(), k));
    out_end();
}

static void run_lui(void) {
    out_begin("lui");
    u64 r;
    __asm__ volatile("lui %0, 0x8000" : "=r"(r)); out(r);
    __asm__ volatile("lui %0, 0x7fff" : "=r"(r)); out(r);
    __asm__ volatile("lui %0, 0xffff" : "=r"(r)); out(r);
    __asm__ volatile("lui %0, 0x1234" : "=r"(r)); out(r);
    out_end();
}

static void run_muldiv(void) {
    u64 hi, lo;
    out_begin("mult");
    for (int i = 0; i < N; i++) { op_mult(val32(), val32(), &hi, &lo); out(hi); out(lo); }
    out_end();
    out_begin("multu");
    for (int i = 0; i < N; i++) { op_multu(val32(), val32(), &hi, &lo); out(hi); out(lo); }
    out_end();
    out_begin("div");
    for (int i = 0; i < N; i++) {
        u64 a = val32(), b = val32();
        if ((s32)b == 0 || ((s32)a == (s32)0x80000000 && (s32)b == -1)) continue;
        op_div(a, b, &hi, &lo); out(hi); out(lo);
    }
    out_end();
    out_begin("divu");
    for (int i = 0; i < N; i++) {
        u64 a = val32(), b = val32();
        if ((u32)b == 0) continue;
        op_divu(a, b, &hi, &lo); out(hi); out(lo);
    }
    out_end();
    out_begin("dmult");
    for (int i = 0; i < N; i++) { op_dmult(val64(), val64(), &hi, &lo); out(hi); out(lo); }
    out_end();
    out_begin("dmultu");
    for (int i = 0; i < N; i++) { op_dmultu(val64(), val64(), &hi, &lo); out(hi); out(lo); }
    out_end();
    out_begin("ddiv");
    for (int i = 0; i < N; i++) {
        u64 a = val64(), b = val64();
        if (b == 0 || (a == 0x8000000000000000ull && b == ~0ull)) continue;
        op_ddiv(a, b, &hi, &lo); out(hi); out(lo);
    }
    out_end();
    out_begin("ddivu");
    for (int i = 0; i < N; i++) {
        u64 a = val64(), b = val64();
        if (b == 0) continue;
        op_ddivu(a, b, &hi, &lo); out(hi); out(lo);
    }
    out_end();
}

// VR4300-specific results (ares; n64-systemtest is the hardware reference to confirm them, docs/TEST_ROMS.md).
static void run_vr4300(void) {
    u64 hi, lo;
    op_div(5, 0, &hi, &lo);
    EXPECT("div 5/0 lo", lo, 0xFFFFFFFFFFFFFFFFull);
    EXPECT("div 5/0 hi", hi, 5);
    op_div(0xFFFFFFFFFFFFFFFBull, 0, &hi, &lo);
    EXPECT("div -5/0 lo", lo, 1);
    EXPECT("div -5/0 hi", hi, 0xFFFFFFFFFFFFFFFBull);
    op_div(0xFFFFFFFF80000000ull, ~0ull, &hi, &lo);
    EXPECT("div min/-1 lo", lo, 0xFFFFFFFF80000000ull);
    EXPECT("div min/-1 hi", hi, 0);
    op_divu(0x80000000ull, 0, &hi, &lo);
    EXPECT("divu x/0 lo", lo, 0xFFFFFFFFFFFFFFFFull);
    EXPECT("divu x/0 hi", hi, 0xFFFFFFFF80000000ull);
    op_ddiv(0x8000000000000000ull, ~0ull, &hi, &lo);
    EXPECT("ddiv min/-1 lo", lo, 0x8000000000000000ull);
    EXPECT("ddiv min/-1 hi", hi, 0);
    op_ddivu(1234, 0, &hi, &lo);
    EXPECT("ddivu x/0 lo", lo, ~0ull);
    EXPECT("ddivu x/0 hi", hi, 1234);
    // SRA on a register that is not a sign-extended word: the whole register shifts, then the low word is kept.
    EXPECT("sra non-sx", sh_sra(0x123456789ABCDEF0ull, 4), 0xFFFFFFFF89ABCDEFull);
    EXPECT("srav non-sx", op_srav(0x123456789ABCDEF0ull, 4), 0xFFFFFFFF89ABCDEFull);
    // SRL takes the low word only
    EXPECT("srl non-sx", sh_srl(0x123456789ABCDEF0ull, 4), 0x0000000009ABCDEFull);
    // ADDU on non-sign-extended inputs adds the low words
    EXPECT("addu non-sx", op_addu(0x1234567880000000ull, 0x0000000180000000ull), 0);
}

int test_main(void) {
    rnd_seed(1);
    run3("daddu", op_daddu, 0);
    run3("dsubu", op_dsubu, 0);
    run3("and", op_and, 0);
    run3("or", op_or, 0);
    run3("xor", op_xor, 0);
    run3("nor", op_nor, 0);
    run3("slt", op_slt, 0);
    run3("sltu", op_sltu, 0);
    run3("addu", op_addu, 1);
    run3("subu", op_subu, 1);
    run_trapping();
    run_varshift();
    run_shift("sll", sh_sll, 1);
    run_shift("srl", sh_srl, 1);
    run_shift("sra", sh_sra, 1);
    run_shift("dsll", sh_dsll, 0);
    run_shift("dsrl", sh_dsrl, 0);
    run_shift("dsra", sh_dsra, 0);
    run_shift("dsll32", sh_dsll32, 0);
    run_shift("dsrl32", sh_dsrl32, 0);
    run_shift("dsra32", sh_dsra32, 0);
    run_imm("addiu", im_addiu, 1);
    run_imm("daddiu", im_daddiu, 0);
    run_imm("slti", im_slti, 0);
    run_imm("sltiu", im_sltiu, 0);
    run_imm("andi", im_andi, 0);
    run_imm("ori", im_ori, 0);
    run_imm("xori", im_xori, 0);
    run_lui();
    run_muldiv();
    run_vr4300();
    return 0;
}
