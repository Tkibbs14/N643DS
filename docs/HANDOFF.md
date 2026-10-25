# N643DS handoff (started 2026-10-25, newest state first)

> **▶ Current state (2026-10-25).** The PC core exists and its CPU is checked. `make -C core` builds `n64emu` with no
> warnings. The CPU suite passes: alu 45 groups, mem 19, branch 4 and fpu 41 match qemu (`-cpu R4000`), plus 31 stated
> VR4300 values, and the cartridge-only exc (110 checks) and cop0 (17). Every planted interpreter bug (1-5) is caught.
> Commercial games boot on the PC: Super Mario 64, Mario Kart 64, Ocarina of Time, Star Fox 64, Paper Mario, GoldenEye
> 007, Banjo-Kazooie and Super Smash Bros. run their CPU code and start graphics and audio tasks (300 fields each);
> RSP tasks complete after a fixed time without doing their work, so nothing is drawn. Banjo-Kazooie starts only two
> graphics tasks in that time (not investigated). Nothing runs on the 3DS yet. The owner's games are outside the
> repository (`C:\Users\tyler\TP3DS\n64\`, extracted from the Downloads archives by `extract.sh` there).

## Next steps, in order

1. **Hardware-derived CPU tests.** Fetch n64-systemtest and PeterLemon's CPU tests (owner approval for the download;
   docs/TEST_ROMS.md) and fix what they find. Unconfirmed VR4300 behaviour carried from ares is marked in
   `vr4300*.c`: SRA / SRAV on non-sign-extended input, LWR's upper half, division by zero, the COP0 and COP2 latches,
   the FPU's QNaN-is-invalid rule, unimplemented-operation cases (CVT out of range, CVT from 64-bit beyond 2^55), FR=1
   single writes keeping the upper word.
2. **Low-level RSP on the PC** (scalar + vector unit), the reference for HLE; RSP test ROMs.
3. **Super Mario 64** (user's cartridge) boots: check the boot path, VI set-up, joybus, then its graphics task through
   the low-level RSP into an RDP command stream.
4. **Graphics HLE** (F3D first), compared byte for byte with the low-level RSP's RDP commands; a software RDP on the PC.
5. **The 3DS host**: the relay needs an application field first (DESIGN.md "Remote runs and gc3ds").

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
