#pragma once
// The machine: memory map, RCP interfaces, cartridge, scheduler (docs/DESIGN.md "Machine model").
#include "../common.h"

// ---- Memory -------------------------------------------------------------------------------------------------------
// Guest memory is stored word-swapped: every aligned 32-bit word is kept in host order, so a 32-bit guest access is a
// plain host load or store; a byte lives at (a ^ 3) and a halfword at (a ^ 2). 64-bit accesses are two words, high
// word first. RDRAM, the RSP's DMEM / IMEM, PIF RAM and the cartridge ROM all use this layout.
static inline u32 m_r32(const u8* m, u32 a) { return *(const u32*)(const void*)(m + a); }
static inline void m_w32(u8* m, u32 a, u32 v) { *(u32*)(void*)(m + a) = v; }
static inline u16 m_r16(const u8* m, u32 a) { return *(const u16*)(const void*)(m + (a ^ 2)); }
static inline void m_w16(u8* m, u32 a, u16 v) { *(u16*)(void*)(m + (a ^ 2)) = v; }
static inline u8 m_r8(const u8* m, u32 a) { return m[a ^ 3]; }
static inline void m_w8(u8* m, u32 a, u8 v) { m[a ^ 3] = v; }
static inline u64 m_r64(const u8* m, u32 a) { return ((u64)m_r32(m, a) << 32) | m_r32(m, a + 4); }
static inline void m_w64(u8* m, u32 a, u64 v) { m_w32(m, a, (u32)(v >> 32)); m_w32(m, a + 4, (u32)v); }
// Byte copy between two word-swapped buffers (any alignment).
void m_copy(u8* dst, u32 da, const u8* src, u32 sa, u32 len);

#define RDRAM_MAX (8u << 20)

typedef struct {
    u8* rdram;
    u32 rdramSize;      // 4 or 8 MB (the Expansion Pak)
    u8 spmem[0x2000];   // DMEM 0x000-0xFFF, IMEM 0x1000-0x1FFF
    u8 pifram[64];
    u8* rom;
    u32 romSize;        // padded to a multiple of 4
    u8 isv[0x10000];    // ISViewer debug window at 0x13FF0000 (homebrew text output)
    u8* save;           // EEPROM / SRAM / FlashRAM contents
    u32 saveSize;
    int saveType;       // SAVE_*
    bool saveDirty;
    bool pal;
} Mem;
extern Mem g_mem;

enum { SAVE_NONE, SAVE_EEPROM4K, SAVE_EEPROM16K, SAVE_SRAM32K, SAVE_SRAM96K, SAVE_FLASH128K };

// Physical-address accesses (the CPU's loads and stores after translation, and the RCP's DMA engines).
u32 bus_r32(u32 pa);
void bus_w32(u32 pa, u32 v, u32 mask);  // mask selects the written byte lanes of the big-endian word
u64 bus_r64(u32 pa);
void bus_w64(u32 pa, u64 v);
u16 bus_r16(u32 pa);
u8 bus_r8(u32 pa);
void bus_w16(u32 pa, u16 v);
void bus_w8(u32 pa, u8 v);

// ---- Interrupts (MI) ----------------------------------------------------------------------------------------------
enum { MI_SP = 1u << 0, MI_SI = 1u << 1, MI_AI = 1u << 2, MI_VI = 1u << 3, MI_PI = 1u << 4, MI_DP = 1u << 5 };
void mi_raise(u32 bits);
void mi_clear(u32 bits);
u32 mi_read(u32 reg);
void mi_write(u32 reg, u32 v, u32 mask);

// ---- Devices: reg is the byte offset inside the device's register block --------------------------------------------
u32 sp_read(u32 reg);
void sp_write(u32 reg, u32 v, u32 mask);
u32 sp_pc_read(u32 reg);
void sp_pc_write(u32 reg, u32 v, u32 mask);
u32 dp_read(u32 reg);
void dp_write(u32 reg, u32 v, u32 mask);
u32 vi_read(u32 reg);
void vi_write(u32 reg, u32 v, u32 mask);
u32 ai_read(u32 reg);
void ai_write(u32 reg, u32 v, u32 mask);
u32 pi_read(u32 reg);
void pi_write(u32 reg, u32 v, u32 mask);
u32 ri_read(u32 reg);
void ri_write(u32 reg, u32 v, u32 mask);
u32 rdreg_read(u32 reg);
void rdreg_write(u32 reg, u32 v, u32 mask);
u32 si_read(u32 reg);
void si_write(u32 reg, u32 v, u32 mask);
void pif_process(void);  // runs the joybus commands in PIF RAM (after an SI write DMA)
u32 cart_read32(u32 pa);
void cart_write32(u32 pa, u32 v, u32 mask);

void sys_reset(void);
void vi_reset(void);
void ai_reset(void);
void pi_reset(void);
void si_reset(void);
void sp_reset(void);
void dp_reset(void);

// ---- Scheduler ----------------------------------------------------------------------------------------------------
// Events fire when the CPU's cycle count reaches their time; the CPU loop runs to the earliest one.
enum { EV_COMPARE, EV_VI_INTR, EV_VI_FIELD, EV_PI_DMA, EV_SI_DMA, EV_AI_BUF, EV_SP_TASK, EV_DP_DONE, EV_N };
void sched_reset(void);
void sched_at(int ev, u64 when);
void sched_cancel(int ev);
u64 sched_next(void);      // the earliest pending event's time (UINT64_MAX when none)
void sched_dispatch(u64 now);
void vi_field_event(void);
void vi_intr_event(void);
void pi_dma_done(void);
void si_dma_done(void);
void ai_buf_event(void);
void sp_task_done(void);
void dp_done_event(void);

// ---- Host hooks ---------------------------------------------------------------------------------------------------
typedef struct {
    u16 buttons;  // N64 order: A B Z START UP DOWN LEFT RIGHT, 0 0 L R C-UP C-DOWN C-LEFT C-RIGHT (bit 15 = A)
    s8 x, y;
    bool present;
} Pad;
extern Pad g_pad[4];
extern void (*g_onField)(void);              // after each VI field
extern void (*g_onIsv)(const char* s, u32 n); // ISViewer text
extern void (*g_onAudio)(const s16* lr, u32 frames, u32 rate);

typedef struct {
    u64 fields, spTasks[8], dpKicks, piDmas, siDmas, aiBufs;
} SysStats;
extern SysStats g_stats;

// VI state the host needs to show a picture
typedef struct {
    u32 ctrl, origin, width, vIntr, vSync, hSync, leap, hStart, vStart, vBurst, xScale, yScale, burst;
    u32 field;
    u64 fieldStart;  // cycles
    u32 cyclesPerField;
} ViState;
extern ViState g_vi;

// ---- Compiled code (src/jit) ----------------------------------------------------------------------------------------
// One byte per 4 KB page of RDRAM: nonzero while the JIT holds code compiled from that page. Every write into RDRAM
// (CPU stores, DMA) checks it and tells the JIT, which drops the page's code.
extern u8 g_codePage[RDRAM_MAX >> 12];
void jit_code_written(u32 pa, u32 len);
static inline void code_range_written(u32 pa, u32 len) {
    for (u32 p = pa >> 12, e = (pa + len - 1) >> 12; p <= e && p < (RDRAM_MAX >> 12); p++)
        if (g_codePage[p]) { jit_code_written(p << 12, 0x1000); }
}

// ---- RSP / RDP --------------------------------------------------------------------------------------------------------
enum { RSP_STUB, RSP_LLE };
extern int g_rspMode;
extern u64 g_rspTotalCycles;
// The RDP's command stream from start to end (RDRAM, or DMEM when xbus), as written to DP_END (src/rdp)
void rdp_process(u32 start, u32 end, bool xbus);
void rdp_reset(void);
typedef struct { u64 commands, triangles, rects, fills, syncFull, bytes; } RdpStats;
extern RdpStats g_rdpStats;
typedef struct { u32 addr, width, size, height; } RdpCi;  // a colour image drawn to (rdp_soft.c)
extern RdpCi g_rdpCi[8];
extern int g_rdpCiCount;
void rdp_ci_clear(void);
void* rdp_soft_state(u32* size);
u32 rdp_z_addr(void);
extern u32 g_rdpOpCount[64];
extern u64 g_rdpTriLog[16][22];
extern int g_rdpTriLogN;
extern u32 g_rdpTriLogId;
extern u64 g_rdpTriLogOm[16][2];

// Host time, for profiling (each frontend provides it): ticks, and ticks a second
u64 host_ticks(void);
extern u64 g_hostTicksPerSec;
extern u64 g_timeRsp, g_timeRdp;  // host ticks in the RSP interpreter (RDP included) and in the RDP
