# Measurements

Console measurements, newest first: relay jobs on the owner's New 2DS XL (`tools/relay.sh`), settled speed from the
job's perf.csv (10 s rows; `speed_pct` is emulated time over real time). The RSP is still a stub (tasks complete
after a fixed time, nothing is drawn), so these are CPU-side figures, not playable speed.

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
