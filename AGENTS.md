# AGENTS.md: working on N643DS

For people and coding agents. Read this whole file before changing anything, then `docs/HANDOFF.md` (the state) and
`docs/DESIGN.md` (the plan).

N643DS is an N64 emulator for the New Nintendo 3DS / New 2DS XL (ARM11 MPCore, 4 cores at 804 MHz, 2 MB shared L2,
PICA200 GPU). The goal is full speed for most of the library with an exact CPU. It follows gc3ds's method and reuses
its 3DS knowledge; gc3ds's `docs/FINDINGS.md`, `docs/research/` and `AGENTS.md` "Hardware facts" apply here.

## Rules

1. **No game data, ever.** No ROMs, PIF ROM, saves of commercial games or extracted assets in the repository, issues
   or pull requests. Tests use programs built from source here, or open-source homebrew fetched by script into an
   ignored directory (docs/TEST_ROMS.md).
2. **Exactness first.** The interpreter (`core/src/cpu/vr4300*.c`) is the CPU reference; a JIT that changes results
   is a bug. The RSP and RDP follow the three-tier model in `docs/DESIGN.md`.
3. **Measure, do not guess.** Performance work is judged on the console with separate whole runs or in-run A/Bs, as in
   gc3ds. Report means and standard errors.
4. **Keep the evidence.** Ideas with redesign potential go into `docs/research/REDESIGN_CANDIDATES.md` with evidence,
   cost, status and a "re-evaluate when" trigger. Never delete an entry; update its status.
5. **Off by default until measured.** New optimizations come behind a switch, off until measured; a measured, exact
   gain is turned on without asking (the owner's standing rule); crash risks and trade-offs go to the owner.
6. **Every checker has a planted bug** that it must catch. A checker whose self-test cannot fail proves nothing.

## Repository map

| Path | What |
|---|---|
| `core/src/cpu/` | VR4300 interpreter: `vr4300.c` (integer unit, COP0, exceptions, Count / Compare), `vr4300_tlb.c` (segments, TLB), `vr4300_fpu.c` (COP1), `vr4300_disasm.c` |
| `core/src/sys/` | `bus.c` (memory map, cartridge, ISViewer), `sched.c`, `mi.c`, `vi.c`, `ai.c`, `pi.c`, `si.c` (PIF joybus), `sp.c` (RSP interface + RDP registers; tasks are stubbed), `ri.c`, `rom.c` (loading, CIC, high-level boot), `system.c` (reset, main loop) |
| `core/src/host/pc_main.c` | PC frontend: headless runs, ISViewer text, VI pictures as PPM, statistics |
| `core/tests/cpu/` | CPU suite: one C source per test, run as a cartridge and under qemu (`run_tests.sh`) |
| `docs/` | DESIGN, HANDOFF, TEST_ROMS, research |

## Toolchain

WSL Debian (the same machine as gc3ds): host gcc, `mipsel-linux-gnu-gcc` / binutils (used with `-EB` for big-endian
n32 test programs), `qemu-mipsn32` (`qemu-user`), Python 3. devkitARM (`/opt/devkitpro`) once the 3DS host exists.

## Checks (run the rows your change touches)

| Change | Checks |
|---|---|
| CPU interpreter (`core/src/cpu/`) | `bash core/tests/cpu/run_tests.sh` (all PASS) and `bash core/tests/cpu/run_tests.sh plant` (every plant caught). New instructions or behaviours get OUT groups (architectural) or EXPECTs (VR4300-specific), and a new behaviour class gets a plant |
| Machine model (`core/src/sys/`) | `make -C core` with no warnings; the CPU suite; the hardware-derived suites once fetched |
| Anything | `make -C core` with no new warnings |

## Code style

- C11, 4-space indent, lines up to ~120 columns, `u8` / `u32` / `s32` / `u64` types (`core/src/common.h`).
- Comments say why. Behaviour not fixed by the manuals names its source (ares, n64-systemtest, n64brew) and stays
  marked until a hardware-derived test confirms it.
- Commit subjects: `type: summary` (`feat`, `fix`, `perf`, `docs`, `tools`, `test`), lower case, the check or
  measurement in the body.
