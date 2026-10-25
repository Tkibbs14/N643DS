# Test programs

No commercial game data is ever part of this repository: no ROMs, no save files of commercial games, no PIF ROM.
Game tests take a cartridge image the user owns through `U64_ROM` (planned) or the command line.

## Our CPU suite (core/tests/cpu)

`bash core/tests/cpu/run_tests.sh` builds each test twice from one C source (big-endian n32, `mipsel-linux-gnu-gcc
-EB`): as a cartridge for u64emu, and as a Linux program for `qemu-mipsn32 -cpu R4000`.

- **OUT lines** are architectural results, hashed per group. They must match qemu. 32-bit instructions get
  sign-extended operands there, since other inputs are UNPREDICTABLE in MIPS III; FPU operands and results stay
  normal, since the VR4300 traps on denormals.
- **EXPECT lines** state a value. They cover VR4300-specific behaviour and exceptions, which only the cartridge build
  can take. Where the value comes from ares rather than the VR4300 manual, the test says so.
- `bash core/tests/cpu/run_tests.sh plant` runs each planted interpreter bug (`U64_PLANT`, `cpu/vr4300.h`); every one
  must fail its test.

| Test | Covers | Groups / checks |
|---|---|---|
| alu | every ALU, shift, multiply and divide instruction; division by zero, SRA / SRAV / SRL / ADDU on non-sign-extended input | 45 groups + 16 EXPECT |
| mem | all loads and stores incl. LWL / LWR / LDL / LDR / SWL / SWR / SDL / SDR | 19 groups + 3 EXPECT |
| branch | every branch and jump: delay slots, likely nullification, links | 4 groups + 8 EXPECT |
| fpu | arithmetic, sqrt, abs / neg / mov, all conversions, all 16 compares incl. NaNs, in all rounding modes, with FCR31 | 41 groups + 4 EXPECT |
| exc | overflow, traps, SYSCALL / BREAK, address errors (data, fetch, delay slot), RI, CpU, interrupts, TLB, FPU traps, LL / SC | cartridge only, 110 checks |
| cop0 | PRId, Random / Wired, write masks, the COP0 latch, Count, FR=0 pairs, FCR0 | cartridge only, 17 checks |

The suite's own expectations come from the same reading of the manuals as the emulator, so they are weaker evidence
than a hardware-derived suite: agreement with qemu checks the architecture, not the VR4300's quirks.

## Hardware-derived suites (not yet fetched)

These are open-source homebrew test ROMs whose expected values were measured on real consoles. They are the
reference for VR4300, RSP and RDP behaviour. They need the owner's approval to download (source, size and license
checked first), go in an ignored directory, and are fetched by a script, never committed.

- **n64-systemtest** (github.com/lemmy-64/n64-systemtest): CPU, COP0, COP1, TLB, exceptions, RSP; reports through
  ISViewer, which u64emu prints.
- **PeterLemon/N64** (github.com/PeterLemon/N64): CPU, RSP and RDP tests with reference pictures.
- Others to evaluate when the RSP and RDP work starts (RDP test suites with captured hardware output).

Both need the cartridge's own IPL3 or a boot that tolerates it; u64emu's high-level boot copies the first megabyte to
the header's entry point, which suits ROMs built with a standard IPL3. libdragon's open IPL3 needs the IPL3 path
(DESIGN.md "Boot").
