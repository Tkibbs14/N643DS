#pragma once
#include "../common.h"

extern volatile bool g_quit;
bool sys_init(u32 rdramSize, int saveType);  // allocates RDRAM and the save memory
void sys_reset(void);
void sys_run(u64 untilCycles);
