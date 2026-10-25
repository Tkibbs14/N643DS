// Cartridge loading and the high-level boot.
//
// The boot does what the PIF and the cartridge's IPL3 leave behind instead of running them (the PIF ROM is not
// redistributable, and IPL3's RDRAM set-up needs RI / RDRAM behaviour not modelled yet): the first megabyte after the
// header is copied to the entry point, osMemSize is stored, and the registers IPL3 passes to the game are set.
#include "rom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sys.h"
#include "../cpu/vr4300.h"

RomInfo g_rom;

static u32 crc32(const u8* p, u32 n) {
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// CRC32 of the IPL3 (ROM 0x40-0xFFF) for the retail CIC chips. Values as published by CIC detection tools; to be
// confirmed against the user's cartridges (docs/TEST_ROMS.md). Unknown IPL3s (homebrew) boot as 6102.
static const struct { u32 crc; int cic; u8 seed; } kCics[] = {
    {0x6170A4A1, 6101, 0x3F}, {0x90BB6CB5, 6102, 0x3F}, {0x0B050EE0, 6103, 0x78},
    {0x98BC2C86, 6105, 0x91}, {0xACC8580A, 6106, 0x85}, {0x009E9EA3, 7102, 0x3F},
};

bool rom_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0x1000 || n > 0x10000000) { fprintf(stderr, "%s: not a cartridge image (%ld bytes)\n", path, n); fclose(f); return false; }
    u32 size = ((u32)n + 3) & ~3u;
    u8* raw = calloc(1, size);
    if (!raw || fread(raw, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(raw); return false; }
    fclose(f);

    // To big-endian byte order: .z64 is already; .v64 swaps each halfword; .n64 is little-endian words.
    u32 magic = ((u32)raw[0] << 24) | ((u32)raw[1] << 16) | ((u32)raw[2] << 8) | raw[3];
    if (magic == 0x37804012u) {
        for (u32 i = 0; i + 1 < size; i += 2) { u8 t = raw[i]; raw[i] = raw[i + 1]; raw[i + 1] = t; }
    } else if (magic == 0x40123780u) {
        for (u32 i = 0; i < size; i += 4) {
            u8 a = raw[i], b = raw[i + 1];
            raw[i] = raw[i + 3]; raw[i + 1] = raw[i + 2]; raw[i + 2] = b; raw[i + 3] = a;
        }
    } else if (magic != 0x80371240u) {
        fprintf(stderr, "%s: unknown header 0x%08x (booting it anyway)\n", path, (unsigned)magic);
    }

    memset(&g_rom, 0, sizeof g_rom);
    g_rom.entry = ((u32)raw[8] << 24) | ((u32)raw[9] << 16) | ((u32)raw[10] << 8) | raw[11];
    memcpy(g_rom.name, raw + 0x20, 20);
    for (int i = 19; i >= 0 && (g_rom.name[i] == ' ' || g_rom.name[i] == 0); i--) g_rom.name[i] = 0;
    memcpy(g_rom.code, raw + 0x3B, 4);
    g_rom.version = raw[0x3F];
    g_rom.ipl3Crc = crc32(raw + 0x40, 0x1000 - 0x40);
    g_rom.cic = 6102;
    g_rom.seed = 0x3F;
    g_rom.cicKnown = false;
    for (size_t i = 0; i < sizeof kCics / sizeof kCics[0]; i++) {
        if (kCics[i].crc == g_rom.ipl3Crc) {
            g_rom.cic = kCics[i].cic;
            g_rom.seed = kCics[i].seed;
            g_rom.cicKnown = true;
        }
    }
    char region = g_rom.code[3];
    g_mem.pal = region == 'D' || region == 'F' || region == 'I' || region == 'P' || region == 'S' || region == 'U' ||
                region == 'X' || region == 'Y';

    // Store word-swapped (sys.h)
    free(g_mem.rom);
    g_mem.rom = malloc(size);
    if (!g_mem.rom) { free(raw); return false; }
    for (u32 i = 0; i < size; i += 4)
        m_w32(g_mem.rom, i, ((u32)raw[i] << 24) | ((u32)raw[i + 1] << 16) | ((u32)raw[i + 2] << 8) | raw[i + 3]);
    g_mem.romSize = size;
    free(raw);
    return true;
}

void rom_set_cic(int cic) {
    for (size_t i = 0; i < sizeof kCics / sizeof kCics[0]; i++)
        if (kCics[i].cic == cic) { g_rom.cic = cic; g_rom.seed = kCics[i].seed; g_rom.cicKnown = true; }
}

void pi_set_dom1(u32 lat, u32 pwd, u32 pgs, u32 rls);

void rom_boot_hle(void) {
    Vr* c = &g_cpu;
    // IPL3 copies itself and the header into DMEM before running; the copy stays there.
    m_copy(g_mem.spmem, 0, g_mem.rom, 0, 0x1000);

    // CIC 6103 and 6106 IPL3s load the game 1 MB / 2 MB below the header's entry point.
    u32 entry = g_rom.entry;
    if (g_rom.cic == 6103) entry -= 0x100000;
    if (g_rom.cic == 6106) entry -= 0x200000;
    u32 dst = entry & 0x1FFFFFFF;
    u32 len = 0x100000;
    if (0x1000 + len > g_mem.romSize) len = g_mem.romSize - 0x1000;
    if (dst + len > g_mem.rdramSize) len = g_mem.rdramSize - dst;
    m_copy(g_mem.rdram, dst, g_mem.rom, 0x1000, len);

    // The boot globals IPL3 stores for libultra (n64brew "IPL3"): osTvType, osRomType, osRomBase, osResetType, osCicId,
    // osVersion, osMemSize (also at 0x3F0 for CIC 6105), then osAppNMIBuffer. libultra builds every PI DMA address
    // from osRomBase. osCicId holds the CIC's number here (unverified against hardware).
    m_w32(g_mem.rdram, 0x300, g_mem.pal ? 0 : 1);
    m_w32(g_mem.rdram, 0x304, 0);
    m_w32(g_mem.rdram, 0x308, 0xB0000000u);
    m_w32(g_mem.rdram, 0x30C, 0);
    m_w32(g_mem.rdram, 0x310, (u32)g_rom.cic);
    m_w32(g_mem.rdram, 0x314, 0);
    m_w32(g_mem.rdram, 0x318, g_mem.rdramSize);
    if (g_rom.cic == 6105) m_w32(g_mem.rdram, 0x3F0, g_mem.rdramSize);

    // PI domain 1 timing from the header's first word
    u32 h = m_r32(g_mem.rom, 0);
    pi_set_dom1(h >> 24 & 0xFF, h >> 16 & 0xFF, h >> 8 & 0x0F, h >> 20 & 0x03);

    // Registers at the game's entry: the values IPL3 hands over (s3-s7) and its stack.
    c->gpr[19] = 0;                       // s3: osRomType (cartridge)
    c->gpr[20] = g_mem.pal ? 0 : 1;       // s4: osTvType (0 PAL, 1 NTSC)
    c->gpr[21] = 0;                       // s5: osResetType (cold)
    c->gpr[22] = g_rom.seed;              // s6: CIC seed
    c->gpr[23] = 0;                       // s7: osVersion
    c->gpr[29] = 0xFFFFFFFFA4001FF0ull;   // sp
    c->gpr[31] = 0xFFFFFFFFA4001550ull;   // ra
    c->gpr[11] = 0xFFFFFFFFA4000040ull;   // t3
    // COP0 as IPL3 leaves it on hardware (PeterLemon/N64 Exceptions/*.png, captured from a console, "before" column):
    // Status CU1 | FR | SR | KX | SX | UX, EPC / ErrorEPC / BadVAddr all ones, Context's BadVPN2 all ones.
    c->cop0[C0_STATUS] = 0x241000E0;
    c->cop0[C0_EPC] = ~0ull;
    c->cop0[C0_ERROREPC] = ~0ull;
    c->cop0[C0_BADVADDR] = ~0ull;
    c->cop0[C0_CONTEXT] = 0x007FFFF0;
    c->cop0[C0_CAUSE] = 0xB000007C;  // BD, CE 3, ExcCode 31 (CP0/COP0Cause: the value after boot)
    c->cop0[C0_LLADDR] = 0xFFFFFFFF;
    c->cop0[C0_WATCHLO] = 0xFFFFFFFB;
    c->cop0[C0_WATCHHI] = 0x0000000F;
    c->cop0[C0_XCONTEXT] = SEXT32(0xFFFFFFF0);
    c->cop0[C0_CONFIG] = 0x7006E463;
    c->pc = SEXT32(entry);
    c->npc = c->pc + 4;
}
