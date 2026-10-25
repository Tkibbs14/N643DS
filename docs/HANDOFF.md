# N643DS handoff (started 2026-10-25, newest state first)

> **▶ Current state (2026-10-25, evening).** The interpreter runs on the PC and on the 2DS.
> - **CPU checks:** our suite passes (alu 45, mem 19, branch 4, fpu 41 groups equal qemu's; exc, cop0) with every
>   plant caught. PeterLemon's hardware-captured CPU tests: **95 of 114 pictures identical** (an earlier commit message,
>   d751773, said 98: wrong, it was written before the full rerun). The rest: TLB / exception / Compare / VI tests that
>   show PASS but print random power-on register contents; SB and SH, identical text with an offset reference; the PI
>   DMA alignment tests (real, open); two timing tests without a reference.
> - **On the 2DS** (relay jobs, `tools/relay.sh`): the CPU test cartridges PASS and 108 of 109 OUT hashes equal qemu's
>   (job 1437); the odd one, cvt.s.l, was a host dependence (fixed in b175120, rerun pending as job 1440). Super Mario
>   64 runs at **4.3% speed, 4.0 MIPS** on the interpreter (job 1438).
> - **Games on the PC:** Super Mario 64, Mario Kart 64, Ocarina of Time, Star Fox 64, Paper Mario, GoldenEye 007,
>   Banjo-Kazooie and Super Smash Bros. run their CPU code and start graphics and audio tasks; RSP tasks are stubbed, so
>   nothing is drawn. The owner's games: `C:\Users\tyler\TP3DS\n64\` (outside the repository; `extract.sh` there).
> - **Relay:** N643DS jobs carry `@n643ds` and are claimed only by `GET /next?app=n643ds` (nakama-backend 9fa948f);
>   gc3ds has a `--switch PATH` job (gc3ds 37282ee) and N643DS too, to hand the 2DS back and forth.

## Next steps, in order

1. **The JIT** on the 2DS (the interpreter's 4 MIPS is ~6% of what games need): reuse gc3ds's ARM emitter and
   register cache (DESIGN.md "Budget").
2. **Low-level RSP on the PC** (scalar + vector unit), the reference for graphics / audio HLE; PeterLemon's RSPTest.
3. **Graphics HLE** (F3D first, Super Mario 64), checked byte for byte against the low-level RSP's RDP commands; a
   software RDP on the PC for pictures.
4. **PI DMA alignment** (PeterLemon DMAAlignment-PI-cart) and n64-systemtest (built from source: Rust + an IPL3 taken
   from the owner's cartridge, kept outside the repository).
5. Still from ares, not yet confirmed by a hardware test: SRA / SRAV on non-sign-extended input, LWR's upper half,
   division by zero, the COP2 latch, the FPU's QNaN-is-invalid rule, CVT out of range, FR=1 single writes.

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
