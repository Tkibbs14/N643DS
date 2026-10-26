# Measurements

Console measurements, newest first: relay jobs on the owner's New 2DS XL (`tools/relay.sh`), settled speed from the
job's perf.csv (10 s rows; `speed_pct` is emulated time over real time). The RSP is still a stub (tasks complete
after a fixed time, nothing is drawn), so these are CPU-side figures, not playable speed.

## The FPU in the JIT, and where the time goes (2026-10-25, night)

Super Mario 64, title head scene, `--jit on --gfx hle --gpu on`, 60 s runs. `--profile on` samples core 0's phase
every ~100 us from core 1 (core/host3ds/profile.c).

| Job | Change | Speed | Profile |
|---|---|---|---|
| 1472 | (before) | 23.8% | |
| 1474 | COP1 arithmetic, compares, conversions, CFC1 / CTC1 and BC1 translated | 24.0% | |
| 1475 | (same build, profiled) | 23.9% | compiled 47.2%, interpreter 29.5%, HLE graphics 18.1%, dispatcher 3.9% |
| 1476 | the FPU fast path also with FCR31's V / Z / O enables set | 30.5% | compiled 65.1%, interpreter 5.0%, HLE graphics 23.4%, dispatcher 5.0% |
| 1477 | graphics tasks on core 2, the CPU waiting at each task's end (`--async on`) | 31.1% | core 0 waited 13.6 s for 14.3 s of worker time: SM64 idles right after submitting a task |
| 1478 | the CPU running on past a task's end (`--async ahead`) | 41-43% | compiled 85.7%, interpreter 6.9%, dispatcher 6.1%; core 0 waited 0.46 s, the worker busy 19.2 s |

- Job 1475's interpreter time was SM64's matrix code (MUL.S / ADD.S at 0x80196570...): the fast path refused every
  operation because SM64 runs with FCR31 = V, Z and O enabled. Only the inexact enable matters to the fast path (it
  completes only when the host raised none of the others).
- Job 1479 (Mario Kart 64, `--async ahead`) hung the 2DS between 10 and 20 s, where the title shows frames without a GPU
  target and the main thread drew RDRAM to the screen while the worker drove the GPU. Since then the main thread
  waits for the worker before drawing RDRAM, and the relay read-back no longer waits for a PPF interrupt;
  not yet re-measured.
- The "interpreted" figure in the JIT statistics counts the slow-path stubs of guarded translations too (every
  load and store has one); the profile's interpreter share is the measure.

## Graphics: HLE and the PICA renderer on the 2DS (2026-10-25, late)

Super Mario 64, `--jit on`, settled speed in the title's Mario-head scene (the logos before it run at ~50%). Audio
tasks are stubbed in every row (no audio HLE yet).

| Job | Graphics | Speed (head scene) | Where the graphics time goes |
|---|---|---|---|
| 1451 | RSP interpreter + software RDP (`--rsp lle`) | ~3.6% | the reference path |
| 1453 / 1454 | graphics HLE + software RDP (`--gfx hle`) | 6.0-12.3% | 46.2 s of 60 s in graphics tasks, of it 44.8 s rasterising (HLE geometry 1.4 s) |
| 1461 | graphics HLE + PICA renderer (`--gfx hle --gpu on`) | 14.1-14.4% | 20.7 s of 40 s in the HLE and the GPU feed (8,402 draw calls, 157,741 triangles) |

- The PICA path draws the title correctly (job 1461's pictures). Then, each measured on the 2DS:

| Job | Change | Super Mario 64 (head scene) | Mario Kart 64 (title) |
|---|---|---|---|
| 1462 | texture lookup skipped while no load / tile / mode changed (`g_rdpTexGen`) | 21.4% (HLE + feed 10.1 s of 40) | |
| 1463 | no host tick per RDP command | 21.9% | |
| 1464 | no vblank pacing in `C3D_FrameBegin` | 22.5% (1,054 fields in 60 s against 1,012) | |
| 1467 | (MK64 measured) | | 5.1% (texture lookups 25.4 s of 40: 25,199 decodes) |
| 1468 | row decoder, word hash, same-size reuse | | 6.3% (no change: the decode was not the cost) |
| 1470 | `svcFlushProcessDataCache` instead of `C3D_TexFlush` (an IPC call to GSP) | | 22-27% |
| 1471 / 1472 | texture cache 192 -> 512 entries (the title's working set thrashed it) | 23.8% (graphics 10.7 s of 60) | 48% (550 decodes in 30 s instead of 69,909) |

- Super Mario 64 is now CPU-bound: half the JIT's compiled instructions still run through the interpreter (job 1472:
  50,336 of 98,787), and the CPU side alone reached ~30% with graphics stubbed (job 1450).
- PC, per task against the interpreter at 600 fields (`--gfx compare`): clearly different pixels Super Mario 64
  1.09%, Mario Kart 64 0.42%, Ocarina of Time 0.32%, Star Fox 64 0, Paper Mario 0.000%, Super Smash Bros. 0.60%,
  Wave Race 64 0.58%, Dr. Mario 64 0.18%, Mario Party 0, Pilotwings 64 1.36%, GoldenEye 007 0.04%.

## The first JIT on the 2DS (2026-10-25)

| Job | Build | Cartridge | Mode | Settled speed | MIPS |
|---|---|---|---|---|---|
| 1438 | 8c1d30ff | Super Mario 64 | interpreter | 4.30% | 4.03 |
| 1441 | e1a30721 | Super Mario 64 | interpreter | 4.46% | 4.18 |
| 1442 | e1a30721 | Super Mario 64 | JIT, no idle skip | 10.0% (2.2x) | 9.4 |
| 1444 | e076a3e5 | Super Mario 64 | JIT, idle loops skipped | 23-24% | 22 |
| 1445 | e076a3e5 | Mario Kart 64 | JIT, idle loops skipped | 24-26% | 23-25 |
| 1450 | a36d3027 | Super Mario 64 | JIT + trapping arithmetic, variable shifts, FPU moves / memory | 28.5-30.8% | 27-29 |

- Super Mario 64 reaches 68% and Mario Kart 64 131% in their first 10 s (boot, logos: mostly idle), then settle.
- JIT statistics (job 1444): of 76,541 compiled instructions, 40,848 still ran through the interpreter; 48.8 M block
  entries in 60 s. The commonest fallbacks on the PC (`n64emu --stats`, 300 fields): loads and stores that leave
  RDRAM (MMIO), the trapping ADD / ADDI / SUB, the variable shifts, and the FPU (arithmetic, moves, LWC1 / SWC1 /
  LDC1 / SDC1). The trapping arithmetic, variable shifts and FPU moves / memory are translated since.
- Job 1449 (same build): the CPU tests pass under the JIT and all 109 OUT hashes equal qemu's.
- Job 1443 (the CPU tests with the JIT, several cartridges in one process) ran the previous cartridge's compiled code:
  fixed by flushing the JIT on every reset (28e8c9f).
