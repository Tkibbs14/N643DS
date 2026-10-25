#pragma once
#include "../common.h"

// High-level graphics microcode (gfx.c): runs the display list of the graphics task in DMEM's OSTask natively.
// False when the microcode is not one it knows (the RSP interpreter runs it).
bool hle_gfx_task(void);
void hle_reset(void);
u64 hle_last_commands(void);  // display-list commands of the last task (for its run time)
typedef struct { u64 tasks, unknown, commands, triangles; } HleStats;
extern HleStats g_hleStats;
extern bool g_hleGfx;  // use it (n64emu --gfx hle|lle)

// HLE against the interpreter task by task (compare.c, n64emu --gfx compare). before: runs the task through gfx.c and
// puts RDRAM and the RDP state back (false: unknown microcode); after (once the interpreter ran it): compares.
bool hle_compare_before(void);
void hle_compare_after(void);
typedef struct { u64 tasks, identical, bad, pixels, diff, big, dumps, zPixels, zDiff; } HleCompareStats;  // bad: over 1% clearly different
extern HleCompareStats g_hleCmp;
extern bool g_hleCompare;

// Where triangles go instead of RDP commands (a GPU renderer, core/host3ds/rdp_pica.c; NULL: RDP triangle commands
// for rdp_soft.c). A clipped, culled convex polygon of n (3..12) vertices in screen pixels; flags as the RDP triangle
// command's id (4 shade, 2 texture, 1 z-buffer), tile in bits 8-10. The state is g_rdp's at the call.
typedef struct { float sx, sy, sz, w, s, t, r, g, b, a; } HleVtx;  // s, t in 10.5 texel units; w > 0 (clip w)
extern void (*g_hleVtxSink)(const HleVtx* v, int n, u32 flags);
