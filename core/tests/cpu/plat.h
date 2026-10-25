// CPU test programs: one C source runs twice, as a cartridge on u64emu and as a Linux n32 program on qemu-mipsn32
// (-cpu R4000), and run_tests.sh diffs the two outputs. OUT lines are architectural results and must match qemu;
// EXPECT lines are VR4300-specific behaviour (or exceptions, which only the cartridge build can take) and are checked
// against a stated value in the cartridge build only. docs/TEST_ROMS.md.
#pragma once

typedef unsigned long long u64;
typedef long long s64;
typedef unsigned int u32;
typedef int s32;
typedef unsigned short u16;
typedef unsigned char u8;

#ifdef PLAT_ROM
#define ON_ROM 1
#else
#define ON_ROM 0
#endif

void plat_write(const char* s, u32 n);
void plat_exit(int code);

void puts_(const char* s);
void put_hex(u64 v);
void put_dec(s64 v);

// Accumulates OUT values per named group; out_end prints one hash line per group (out_verbose(1): every value).
void out_begin(const char* group);
void out(u64 v);
void out_end(void);
void out_verbose(int on);

// EXPECT: cartridge build only; prints FAIL with the line on mismatch.
void expect_(const char* what, int line, u64 got, u64 want);
#define EXPECT(what, got, want) expect_(what, __LINE__, (u64)(got), (u64)(want))
extern int g_failures;

// Deterministic values: random 64-bit, random sign-extended 32-bit, and edge cases.
u64 rnd(void);
u64 val64(void);
u64 val32(void);  // sign-extended 32-bit
void rnd_seed(u64 s);

int test_main(void);  // each test program
