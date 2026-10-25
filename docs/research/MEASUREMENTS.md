# Measurements

Console measurements, newest first: relay jobs on the owner's New 2DS XL (`tools/relay.sh`), settled speed from the
job's perf.csv (10 s rows; `speed_pct` is emulated time over real time). The RSP is still a stub (tasks complete
after a fixed time, nothing is drawn), so these are CPU-side figures, not playable speed.

## Graphics: HLE and the PICA renderer on the 2DS (2026-10-25, late)

Super Mario 64, `--jit on`, settled speed in the title's Mario-head scene (the logos before it run at ~50%). Audio
tasks are stubbed in every row (no audio HLE yet).

| Job | Graphics | Speed (head scene) | Where the graphics time goes |
|---|---|---|---|
| 1451 | RSP interpreter + software RDP (`--rsp lle`) | ~3.6% | the reference path |
| 1453 / 1454 | graphics HLE + software RDP (`--gfx hle`) | 6.0-12.3% | 46.2 s of 60 s in graphics tasks, of it 44.8 s rasterising (HLE geometry 1.4 s) |
| 1461 | graphics HLE + PICA renderer (`--gfx hle --gpu on`) | 14.1-14.4% | 20.7 s of 40 s in the HLE and the GPU feed (8,402 draw calls, 157,741 triangles) |

- The PICA path draws the title correctly (job 1461's pictures); its cost is now on the CPU side of the feed. The
  first suspect is the texture lookup, which hashes up to 4 KB of TMEM per polygon and tile.
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
