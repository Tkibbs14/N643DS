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
| `core/src/sys/` | `bus.c` (memory map, cartridge, ISViewer), `sched.c`, `mi.c`, `vi.c`, `ai.c`, `pi.c`, `si.c` (PIF joybus), `sp.c` (SP interface, RSP task timing, RDP registers), `ri.c`, `rom.c` (loading, CIC, high-level boot), `system.c` (reset, main loop) |
| `core/src/jit/` | VR4300 -> ARMv6K JIT, exact against the interpreter (`arm_emit.h` from gc3ds) |
| `core/src/rsp/` | RSP interpreter: scalar unit (`rsp.c`), vector unit and vector loads / stores (`rsp_vu.c`) |
| `core/src/rdp/` | RDP command stream (`rdp.c`), the software renderer into RDRAM (`rdp_soft.c`, state shared through `rdp.h`), the combiner as PICA TEV stages (`cc_pica.c`) and its checker (`cc_check.c`) |
| `core/src/hle/` | high-level graphics microcode (`gfx.c`: F3D, F3DEX, F3DEX2 / F3DZEX and variants) and its per-task comparison with the interpreter (`compare.c`) |
| `core/host3ds/` | the 3DS program: menu, display, relay client (from gc3ds), JIT host services, the RDP on the PICA200 (`rdp_pica.c`, `n64pass.v.pica`) |
| `core/tools/` | `arm_build.sh` / `jit_check.sh` (the JIT under qemu-arm), `peterlemon.py` (hardware-captured test ROMs), `contact.py` |
| `tools/relay.sh` | remote 2DS runs (`@n643ds` jobs) |
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
| JIT (`core/src/jit/`) | `bash core/tools/jit_check.sh` (CPU test cartridges identical with and without the JIT), `jit_check.sh plant` (plant 6 caught), and `FIELDS=60 jit_check.sh GAMES` (end states identical) |
| RSP / RDP | `python3 core/tools/peterlemon.py third_party/testroms/peterlemon/RSPTest` (and `RDP`): no picture that matched the hardware capture may stop matching |
| Graphics HLE (`core/src/hle/`) | `n64emu ROM --fields 600 --gfx compare --stats` over the games in `docs/HANDOFF.md` (per task, HLE against the interpreter from the same state): no game's "clearly different" share or count of tasks over 1% may grow. `N64_HLE_DUMPDIR=dir` writes differing tasks' pictures; `N64_HLE_CMPLOG=1` logs each task, `N64_HLE_TRACE=task` its commands |
| PICA combiner (`core/src/rdp/cc_pica.c`) | `n64emu ROM --fields 600 --gfx hle --cc-check` (every mode the run drew with: 0 "exact but differing") and with `N64_CC_PLANT=1` (must report differences) |
| Anything | `make -C core` and `bash core/host3ds/build.sh` with no new warnings |

## Code style

- C11, 4-space indent, lines up to ~120 columns, `u8` / `u32` / `s32` / `u64` types (`core/src/common.h`).
- Comments say why. Behaviour not fixed by the manuals names its source (ares, n64-systemtest, n64brew) and stays
  marked until a hardware-derived test confirms it.
- Commit subjects: `type: summary` (`feat`, `fix`, `perf`, `docs`, `tools`, `test`), lower case, the check or
  measurement in the body.
