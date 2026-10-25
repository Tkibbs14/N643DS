# N643DS handoff (started 2026-10-25, newest state first)

> **▶ Current state (2026-10-25, night).** Graphics run natively: a high-level graphics microcode and a PICA200
> renderer. Super Mario 64's title draws on the 2DS's own GPU (relay job 1461) at ~14% speed in the head scene.
> - **Graphics HLE** (`src/hle/gfx.c`): F3D (Fast3D 2.0D / 2.0G), F3DEX 0.95 / 1.x, F3DEX2 / F3DZEX, plus Wave Race 64's
>   variant (indices x5, picked by a hash of the microcode text) and GoldenEye's TRI4. Per task against the
>   interpreter (`n64emu --gfx compare`, 600 fields), clearly different pixels: SM64 1.09%, MK64 0.42%, OoT 0.32%,
>   Star Fox 0, Paper Mario 0.000%, Smash 0.60%, Wave Race 0.58%, Dr. Mario 0.18%, Mario Party 0, Pilotwings 1.36%,
>   GoldenEye 0.04%. Not exact yet: float arithmetic (DESIGN.md "Exactness model").
> - **PICA renderer** (`core/host3ds/rdp_pica.c`, `--gfx hle --gpu on`, off by default): render target per colour
>   image, the combiner as TEV stages (`src/rdp/cc_pica.c`; `--cc-check`: SM64's 8 modes exact, the plant caught),
>   textures from TMEM. Not yet: writing pictures back to RDRAM (framebuffer effects), fog in the blender, triangles
>   from microcodes the HLE does not know, a measured default.
> - **Open questions:** SM64's title "TM" differs between HLE and the interpreter (the interpreter's looks stretched:
>   a perspective-texture precision question for the reference path, needs a hardware capture). Banjo-Kazooie's
>   audio boot (zeros at RDRAM 0x283280).
> - **RSP / RDP / CPU / JIT / relay:** as in the log below (RSPTest 35 of 56 identical; PeterLemon CPU 95 of 114; JIT
>   exact; relay jobs `@n643ds`).

## Next steps, in order

1. **Speed of the GPU path** (job 1461: the HLE and the GPU feed take half the time): skip the texture hash when TMEM
   and the tile did not change (a load generation counter), then profile the feed on the 2DS; then measure and make
   `--gpu on` the default for by-hand runs.
2. **GPU renderer coverage:** pictures back to RDRAM when the CPU or a texture reads them, the 2-cycle fog blend as a
   TEV stage, the interpreter's triangles (microcodes the HLE does not know: Diddy Kong Racing, Banjo-Kazooie).
3. **Audio HLE** (the standard ABI, compared sample for sample with the interpreter) and output through ndsp.
4. **Exact HLE:** the microcodes' fixed-point arithmetic (F3DEX2 disassembly) so the RDP command stream matches.
5. RDP accuracy (PeterLemon's RDP ROMs, the 474 vs 480 rows), Banjo-Kazooie, JIT FPU arithmetic / register cache /
   block linking, PI DMA alignment, n64-systemtest.

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
