// JIT host services on the 3DS, as gc3ds does them (core/host3ds/jit_3ds.c there): executable memory through
// svcControlProcessMemory (Luma3DS allows RWX for homebrew), the data cache flushed in our own process, and the
// instruction cache invalidated whole through Luma3DS's svc 0x94 (gc3ds measured the ranged alternatives crashing).
#include <3ds.h>
#include <malloc.h>
#include "../src/jit/jit.h"

static Handle g_proc;

static void __attribute__((naked, noinline)) luma_invalidate_entire_icache(void) {
    __asm__ volatile("svc 0x94\n\tbx lr\n");
}

void* jit_host_alloc_exec(u32 bytes) {
    bytes = (bytes + 0xFFF) & ~0xFFFu;
    void* p = memalign(0x1000, bytes);
    if (!p) return NULL;
    if (!g_proc && R_FAILED(svcDuplicateHandle(&g_proc, CUR_PROCESS_HANDLE))) return NULL;
    if (R_FAILED(svcControlProcessMemory(g_proc, (u32)p, (u32)p, bytes, MEMOP_PROT,
                                         MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE))) return NULL;
    return p;
}

void jit_host_sync_icache(void* start, u32 bytes) {
    svcFlushProcessDataCache(g_proc, (u32)start, bytes);
    luma_invalidate_entire_icache();
}
