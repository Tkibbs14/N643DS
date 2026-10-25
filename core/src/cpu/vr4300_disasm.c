// A small VR4300 disassembler for traces (--trace N). Mnemonics and operands only; no pseudo-instructions.
#include <stdio.h>
#include "vr4300.h"

static const char* const R[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4",
                                  "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9",
                                  "k0", "k1", "gp", "sp", "s8", "ra"};

static const char* const SPECIAL[64] = {
    "sll", 0, "srl", "sra", "sllv", 0, "srlv", "srav", "jr", "jalr", 0, 0, "syscall", "break", 0, "sync",
    "mfhi", "mthi", "mflo", "mtlo", "dsllv", 0, "dsrlv", "dsrav", "mult", "multu", "div", "divu", "dmult", "dmultu",
    "ddiv", "ddivu", "add", "addu", "sub", "subu", "and", "or", "xor", "nor", 0, 0, "slt", "sltu", "dadd", "daddu",
    "dsub", "dsubu", "tge", "tgeu", "tlt", "tltu", "teq", 0, "tne", 0, "dsll", 0, "dsrl", "dsra", "dsll32", 0,
    "dsrl32", "dsra32"};

static const char* const MAIN[64] = {
    0, 0, "j", "jal", "beq", "bne", "blez", "bgtz", "addi", "addiu", "slti", "sltiu", "andi", "ori", "xori", "lui",
    "cop0", "cop1", "cop2", 0, "beql", "bnel", "blezl", "bgtzl", "daddi", "daddiu", "ldl", "ldr", 0, 0, 0, 0,
    "lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr", "lwu", "sb", "sh", "swl", "sw", "sdl", "sdr", "swr", "cache",
    "ll", "lwc1", "lwc2", 0, "lld", "ldc1", "ldc2", "ld", "sc", "swc1", "swc2", 0, "scd", "sdc1", "sdc2", "sd"};

static const char* const REGIMM[32] = {"bltz", "bgez", "bltzl", "bgezl", 0, 0, 0, 0, "tgei", "tgeiu", "tlti",
                                       "tltiu", "teqi", 0, "tnei", 0, "bltzal", "bgezal", "bltzall", "bgezall"};

const char* vr_disasm(u32 op, u64 pc, char* buf, size_t n) {
    int rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
    s16 imm = (s16)op;
    u32 primary = op >> 26;
    unsigned btarget = (unsigned)(pc + 4 + ((s64)imm << 2));
    if (op == 0) { snprintf(buf, n, "nop"); return buf; }
    if (primary == 0) {
        const char* m = SPECIAL[op & 63];
        if (!m) { snprintf(buf, n, "special?"); return buf; }
        u32 f = op & 63;
        if (f < 4 || (f >= 0x38)) snprintf(buf, n, "%s %s, %s, %d", m, R[rd], R[rt], sa);
        else if (f < 8 || (f >= 0x14 && f <= 0x17)) snprintf(buf, n, "%s %s, %s, %s", m, R[rd], R[rt], R[rs]);
        else if (f == 8) snprintf(buf, n, "jr %s", R[rs]);
        else if (f == 9) snprintf(buf, n, "jalr %s, %s", R[rd], R[rs]);
        else if (f >= 0x10 && f <= 0x13) snprintf(buf, n, "%s %s", m, (f & 1) ? R[rs] : R[rd]);
        else if (f >= 0x18 && f <= 0x1F) snprintf(buf, n, "%s %s, %s", m, R[rs], R[rt]);
        else if (f >= 0x30 && f <= 0x36) snprintf(buf, n, "%s %s, %s", m, R[rs], R[rt]);
        else if (f == 0x0C || f == 0x0D || f == 0x0F) snprintf(buf, n, "%s", m);
        else snprintf(buf, n, "%s %s, %s, %s", m, R[rd], R[rs], R[rt]);
        return buf;
    }
    if (primary == 1) {
        const char* m = REGIMM[rt];
        if (!m) snprintf(buf, n, "regimm?");
        else if (rt >= 8 && rt <= 14) snprintf(buf, n, "%s %s, %d", m, R[rs], imm);
        else snprintf(buf, n, "%s %s, 0x%08x", m, R[rs], btarget);
        return buf;
    }
    const char* m = MAIN[primary];
    if (!m) { snprintf(buf, n, "?"); return buf; }
    if (primary == 2 || primary == 3) {
        snprintf(buf, n, "%s 0x%08x", m, (unsigned)(((pc + 4) & 0xF0000000) | ((op & 0x03FFFFFF) << 2)));
    } else if (primary == 4 || primary == 5 || primary == 0x14 || primary == 0x15) {
        snprintf(buf, n, "%s %s, %s, 0x%08x", m, R[rs], R[rt], btarget);
    } else if (primary == 6 || primary == 7 || primary == 0x16 || primary == 0x17) {
        snprintf(buf, n, "%s %s, 0x%08x", m, R[rs], btarget);
    } else if (primary == 0x0F) {
        snprintf(buf, n, "lui %s, 0x%04x", R[rt], (u16)op);
    } else if (primary >= 0x08 && primary <= 0x19) {
        if (primary >= 0x10 && primary <= 0x12) {
            snprintf(buf, n, "%s rs=%d rt=%d rd=%d fn=0x%02x", m, rs, rt, rd, (unsigned)(op & 63));
        } else if (primary >= 0x0C && primary <= 0x0E) {
            snprintf(buf, n, "%s %s, %s, 0x%04x", m, R[rt], R[rs], (u16)op);
        } else {
            snprintf(buf, n, "%s %s, %s, %d", m, R[rt], R[rs], imm);
        }
    } else if (primary == 0x31 || primary == 0x35 || primary == 0x39 || primary == 0x3D) {
        snprintf(buf, n, "%s f%d, %d(%s)", m, rt, imm, R[rs]);
    } else {
        snprintf(buf, n, "%s %s, %d(%s)", m, R[rt], imm, R[rs]);
    }
    return buf;
}
