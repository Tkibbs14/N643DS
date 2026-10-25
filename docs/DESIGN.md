# N643DS design

N643DS is an N64 emulator for the New Nintendo 3DS / New 2DS XL. It follows gc3ds's method (exact reference,
measured switches, evidence kept) and reuses its 3DS knowledge: the ARM11 JIT techniques, the PICA200 backend's
lessons, the relay and the measurement tools. This file is the plan; `docs/HANDOFF.md` is the state.

## Goal

Full speed for most of the large N64 library on a New 2DS XL, with the CPU exact. "Every game" is not the goal: games
with custom microcodes or heavy framebuffer effects come after the common ones (see "Scope").

## Budget (why this can work, and where it is tight)

- **CPU.** The VR4300 runs at 93.75 MHz, one instruction a cycle at best; games run maybe 60-75 M instructions a
  second under load. gc3ds's JIT retires roughly 80 M Gekko instructions a second on core 0 (about 10 core-0 cycles
  per guest instruction, AGENTS.md there). Feasible, not free: idle-loop skipping and a lean JIT are required.
  Most game code is compiled for 32-bit values, so 64-bit registers can usually be tracked as sign-extended words.
- **RSP.** An 8-lane, 16-bit vector unit at 62.5 MHz. The ARM11 has 2-lane 16-bit SIMD only: running RSP microcode
  instruction by instruction is out of budget, so graphics and audio microcode are replaced by native code (HLE).
- **GPU.** The PICA200 is far beyond the RDP's fill and triangle rate. gc3ds draws Twilight Princess frames in 5-15
  ms. The N64's colour combiner, (A-B)*C+D in one or two cycles, maps onto PICA combiner stages more directly than
  the GameCube's TEV does.
- **Memory.** RDRAM is 4 or 8 MB; a New 3DS application has 124 MB (178 MB in the larger mode, 3dbrew).

## Exactness model (adopted 2026-10-25)

gc3ds's rule is that the interpreter is the reference and everything else must match it. Here that holds for the CPU,
but a PICA200 cannot reproduce the RDP bit for bit, so the proposal is three tiers:

| Part | Reference | Bar |
|---|---|---|
| CPU (interpreter, JIT) | the VR4300 interpreter, itself checked against qemu and hardware-derived test ROMs | exact: same instructions, same state |
| RSP high-level code | a low-level RSP interpreter on the PC running the real microcode | exact at the RSP's outputs: the same RDP command stream for graphics, the same samples for audio |
| RDP on the PICA | a software RDP on the PC | visual: within a stated tolerance per test, differences listed |

The middle tier is what keeps HLE honest: the RSP's output (RDP commands, audio samples) is data that can be compared
byte for byte, so the high-level microcode gets a real checker instead of "looks right".

## Machine model (core/src/sys)

- **Memory layout.** RDRAM, DMEM / IMEM, PIF RAM and the ROM are stored word-swapped: each 32-bit word in host order,
  so a guest word access is a plain host load, a byte is at `a ^ 3`, a halfword at `a ^ 2` (sys.h). The JIT's loads
  will be single `ldr` / `ldrb` instructions with an XOR folded into the address.
- **Scheduler.** Fixed events (Compare, VI line interrupt, VI field, PI / SI DMA, AI buffer, RSP task, RDP). The CPU
  runs to the earliest; a device that schedules an earlier event lowers the CPU's limit.
- **Timing (approximate, to be measured).** One cycle per instruction; Count at half the CPU clock; VI field length
  from the VI registers; PI DMA 9 cycles a byte; SI 6,000 cycles a transfer; RSP tasks a fixed 150,000 (graphics) /
  40,000 (audio) cycles until the high-level microcode exists. Each constant is a candidate for measurement against
  hardware-derived timing tests.

## Boot

The PIF ROM is not redistributable, and running a cartridge's IPL3 needs RDRAM / RI behaviour not modelled yet, so
the boot is high-level (sys/rom.c): IPL3's copy to DMEM, the first megabyte to the entry point (minus 1 MB / 2 MB for
CIC 6103 / 6106), osMemSize, and the registers IPL3 hands over. Known gaps: CIC 6105's challenge, libdragon's open
IPL3 (its loader is not the 1 MB copy), and cartridges whose IPL3 is not in the CRC table (they boot as 6102).

## RSP plan

1. A low-level RSP interpreter (scalar unit + vector unit), PC only, as the reference.
2. Graphics HLE for F3D / F3DEX / F3DEX2 (the bulk of the library), output: the RDP command stream, compared with (1).
3. Audio HLE for the standard ABI, compared with (1) sample for sample.
4. Then the special microcodes by measured demand (Rare's, Factor 5's, S2DEX).

## RDP / graphics plan

The high-level graphics microcode produces triangles directly for the PICA (the RDP command stream exists for the
checker, not as the 3DS path). Combiner and blender modes map to PICA combiner stages and blend state; textures
(RGBA16/32, IA, I, CI4/CI8 with TLUT) convert to PICA formats through a TMEM model with a texture cache keyed by
content. Framebuffer effects (the CPU reading the picture, copies into textures) are the known hard part: gc3ds's
EFB-copy work is the closest precedent.

## 3DS layout (planned; from gc3ds's measurements)

Core 0 runs the CPU (JIT). Core 2 runs the graphics HLE and builds PICA command lists. Core 3 does vertex work and
triangle set-up. Core 1 (80% for the application) runs audio HLE, the log writer and the relay client. A second core
reading emulated RAM evicts core 0's working set from the shared 2 MB L2 (gc3ds: core 0 -6%), so sharing is designed
in from the start.

## Remote runs and gc3ds

The relay protocol (gc3ds `docs/REMOTE_RUNS.md`) installs a job's build over the running program's own .3dsx. Before
N643DS builds go through the relay, jobs need an application field so a gc3ds client never installs an N643DS
build (and the reverse), and N643DS keeps its own SD directory (`sdmc:/N643DS/`). That change touches the relay
server and both clients and is not made yet.

## Scope order

1. CPU interpreter checked (done: core/tests/cpu, docs/TEST_ROMS.md).
2. The hardware-derived test ROMs pass on the PC.
3. Low-level RSP on the PC; Super Mario 64 boots and its RSP tasks run.
4. Graphics HLE (F3D) checked against it; a software RDP on the PC for pictures.
5. The 3DS host: interpreter first, then the JIT (reusing gc3ds's emitter), then the PICA path.
6. Audio HLE.
7. Breadth: F3DEX2 (Ocarina, Majora, Mario Kart), the TLB and Rare-microcode games (GoldenEye, Perfect Dark).
