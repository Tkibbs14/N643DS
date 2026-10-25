#include "exc.h"

volatile Exc g_exc;
extern u32 exc_stub[], exc_stub_end[];

void exc_install(void) {
    static const u32 vecs[] = {0x000, 0x080, 0x180};
    u32 n = (u32)(exc_stub_end - exc_stub);
    for (int v = 0; v < 3; v++) {
        volatile u32* dst = (volatile u32*)(0x80000000u + vecs[v]);
        for (u32 i = 0; i < n; i++) dst[i] = exc_stub[i];
        dst[n - 1] = (exc_stub[n - 1] & 0xFFFF0000u) | vecs[v];  // ori k1, zero, vector
    }
}
