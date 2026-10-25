#pragma once
#include "plat.h"

typedef struct {
    u32 count, cause;
    u64 epc, badvaddr;
    u32 status, vector;
    u64 resume;  // where the handler returns next (0: past the faulting instruction)
} Exc;
extern volatile Exc g_exc;
void exc_install(void);

#define EXCCODE(c) (((c) >> 2) & 31)

static inline u32 c0_status(void) { u32 v; __asm__ volatile("mfc0 %0, $12" : "=r"(v)); return v; }
static inline void c0_set_status(u32 v) { __asm__ volatile("mtc0 %0, $12\n\tnop\n\tnop" : : "r"(v)); }
static inline u32 c0_count(void) { u32 v; __asm__ volatile("mfc0 %0, $9" : "=r"(v)); return v; }
