#pragma once
// Sampling where core 0's time goes (profile.c, job option --profile on).
void profile_start(void);
void profile_stop(void (*log)(const char* fmt, ...));
