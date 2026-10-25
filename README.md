# N643DS

An N64 emulator for homebrew-enabled **New Nintendo 3DS / New 2DS XL** systems, built the way gc3ds (the GameCube
emulator for the same hardware) is: an exact CPU reference, every optimization measured on a real New 2DS XL, and the
evidence kept.

**Status: started 25 October 2026. Not a usable emulator yet.** What exists runs on a PC:

- A VR4300 interpreter (integer unit, COP0, TLB, FPU) that passes a CPU test suite checked against qemu and against
  stated VR4300 behaviour (`docs/TEST_ROMS.md`).
- The machine around it: memory map, MI / VI / AI / PI / SI / RSP / RDP interfaces, the PIF's joybus (controllers,
  EEPROM), SRAM, the ISViewer debug port, a high-level boot.
- On the 2DS (`core/host3ds`): the interpreter runs cartridges from `sdmc:/n643ds/roms` at ~4 MIPS (Super Mario 64:
  4.3% speed); remote runs through gc3ds's relay (`tools/relay.sh`).
- No RSP or RDP yet: games boot and run their CPU code (Super Mario 64, Mario Kart 64, Ocarina of Time, Star Fox 64,
  Paper Mario, GoldenEye 007, Banjo-Kazooie and Super Smash Bros. all start their graphics and audio tasks), but RSP
  tasks complete without doing their work, so nothing is drawn. That is the next step (`docs/DESIGN.md`).

**Bring your own games.** N643DS contains no game data, BIOS or firmware, and never will. Use dumps of cartridges
you own.

## Build and run (Linux or WSL)

```bash
make -C core                                     # core/build/n64emu
core/build/n64emu game.z64 --fields 600 --out out --shot-every 60 --stats
bash core/tests/cpu/run_tests.sh                 # the CPU suite (needs mipsel-linux-gnu-gcc and qemu-user)
bash core/tests/cpu/run_tests.sh plant           # its planted-bug self-test
```

`.z64`, `.v64` and `.n64` byte orders are accepted. Options: `--save none|eep4k|eep16k|sram|sram96k|flash`, `--4mb`
(no Expansion Pak), `--cic N`, `--trace N` (disassemble the first N instructions), `--until-isv` (stop at a PASS /
FAIL line from a test ROM).

## Documents

| | |
|---|---|
| `AGENTS.md` | rules, repository map, checks (read first) |
| `docs/DESIGN.md` | the plan: budget, exactness model, machine model, RSP / RDP / 3DS plans, scope order |
| `docs/HANDOFF.md` | the current state and the next steps |
| `docs/TEST_ROMS.md` | the CPU suite and the hardware-derived suites to adopt |
| `docs/research/` | measurements and redesign candidates (empty until the 3DS work starts) |

## License

GPL-2.0-or-later (`LICENSE`), as gc3ds.
