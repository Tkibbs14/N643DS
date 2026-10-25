# N643DS handoff (started 2026-10-25, newest state first)

> **▶ Current state (2026-10-25, late).** Games render: the RSP interpreter runs the games' own microcode into a
> software RDP, on the PC and (slowly) on the 2DS.
> - **Pictures (PC, `core/out`):** Super Mario 64 (logo, title), Mario Kart 64 (title), Ocarina of Time, Star Fox 64,
>   Paper Mario, Super Smash Bros. and Wave Race 64 (openings); GoldenEye 007 draws one object; Banjo-Kazooie stays
>   black (its first audio task boots from zeros at RDRAM 0x283280: open). On the 2DS (relay job 1451, `--rsp lle
>   --jit on`) Super Mario 64's logo and title render on the device at ~4% speed (the reference path).
> - **RSP** (`src/rsp`): PeterLemon's RSPTest, 35 of 56 pictures identical to the hardware captures on the first run;
>   most of the rest show the same values with different line spacing (474 vs 480 rows in our picture), the
>   reserved VACC-family opcodes accumulate on hardware (not modelled), SP DMA alignment and XBUS draw order open.
> - **RDP** (`src/rdp`): a reference renderer (one sample per pixel, no coverage / dither), not yet measured against
>   PeterLemon's RDP ROMs (fetched, 127 of them).
> - **CPU:** our suite passes with every plant caught; PeterLemon's CPU tests 95 of 114 identical (an earlier commit
>   message, d751773, said 98: wrong). **JIT:** exact against the interpreter (`core/tools/jit_check.sh`); on the 2DS
>   Super Mario 64's CPU side runs at 28.5-30.8% with the RSP stubbed (MEASUREMENTS.md).
> - **Relay:** N643DS jobs carry `@n643ds` (nakama-backend 9fa948f); `tools/relay.sh frames JOB` fetches pictures a job
>   sent (`--shots N`, `cmd JOB shot`). New builds install and restart by themselves.

## Next steps, in order

1. **Speed on the 2DS:** graphics microcode HLE (F3D / F3DEX / F3DEX2) producing the same RDP command stream as the
   RSP interpreter (byte-compared on the PC: DESIGN.md "Exactness model"), then the RDP on the PICA200 instead of
   the software renderer, then audio HLE and output (ndsp).
2. **RDP accuracy:** PeterLemon's RDP ROMs through `core/tools/peterlemon.py`; the picture height (474 vs 480 rows).
3. **Banjo-Kazooie** (zeros where its RSP boot code should be), GoldenEye's mostly black picture.
4. **JIT:** FPU arithmetic natively, a register cache, block linking.
5. PI DMA alignment; n64-systemtest (built from source); the ares-derived CPU behaviours still unconfirmed.

## Owner decisions (2026-10-25)

- "You can do anything you'd like": the three-tier exactness model (DESIGN.md) is adopted; the hardware-derived test
  ROMs may be fetched; the repository is named N643DS (github.com/Tkibbs14/N643DS) and is public.

## Log

### 2026-10-25: repository, interpreter, machine model, CPU suite

- Repository created next to gc3ds (`C:\Users\tyler\TP3DS\N643DS`).
- VR4300 interpreter: all MIPS III integer instructions, COP0 with the TLB and exceptions (TLB refill vectors,
  delay-slot EPC / BD, nested EXL), Count / Compare through the scheduler, COP1 on the host FPU with the VR4300's
  denormal and NaN rules and FCR31 cause / flag / enable handling.
- Machine: word-swapped memory, scheduler, MI, VI (field length from its registers), AI (two-entry FIFO), PI DMA, SI
  with the PIF joybus (controllers, EEPROM 4K / 16K), SRAM, the ISViewer window, SP DMA and status with stubbed tasks,
  RDP registers, high-level boot with CIC detection by IPL3 CRC (6101 / 6102 / 6103 / 6105 / 6106 / 7102; the CRC
  values are as published, unconfirmed against cartridges).
- CPU suite: one C source per test, built big-endian n32 as a cartridge and as a Linux program; qemu-mipsn32 is the
  architectural oracle, EXPECTs carry VR4300-specific values. The first runs passed, so planted bugs were added before
  trusting them: each of the five is caught.
