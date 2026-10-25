#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define SEXT32(x) ((u64)(s64)(s32)(u32)(x))
#define SEXT16(x) ((u64)(s64)(s16)(u16)(x))
#define SEXT8(x) ((u64)(s64)(s8)(u8)(x))

#if defined(__GNUC__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

// The CPU's clock (PClock) and the clocks derived from it, NTSC.
#define CPU_HZ 93750000u
#define VI_CLOCK_NTSC 48681812u
#define VI_CLOCK_PAL 49656530u
