#pragma once
#include "../common.h"

typedef struct {
    char name[21];
    char code[5];  // category, two-letter id, region
    u8 version;
    u32 entry;
    u32 ipl3Crc;
    int cic;
    u8 seed;
    bool cicKnown;
} RomInfo;
extern RomInfo g_rom;

bool rom_load(const char* path);
void rom_set_cic(int cic);
void rom_boot_hle(void);
