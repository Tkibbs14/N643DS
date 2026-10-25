// JIT host services for 32-bit ARM Linux (the PC frontend under qemu-arm, core/tools/arm_build.sh).
#if defined(__arm__) && !defined(__3DS__)
#include <sys/mman.h>
#include "../jit/jit.h"

void* jit_host_alloc_exec(u32 bytes) {
    void* p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void jit_host_sync_icache(void* start, u32 bytes) { __builtin___clear_cache((char*)start, (char*)start + bytes); }
#endif
