# N643DS

An N64 emulator for homebrew-enabled **New Nintendo 3DS / New 2DS XL** systems, built the way gc3ds (the GameCube
emulator for the same hardware) is: an exact CPU reference, every optimization measured on a real New 2DS XL, and the
evidence kept.

**Status: started 25 October 2026. Not a usable emulator yet.** Games boot and draw their opening screens:

- A VR4300 interpreter (integer unit, COP0, TLB, FPU), checked against qemu, stated VR4300 behaviour and
  PeterLemon's hardware-captured tests (`docs/TEST_ROMS.md`), and an ARMv6K JIT that is exact against it.
- The machine: memory map, MI / VI / AI / PI / SI / SP / DP interfaces, the PIF's joybus (controllers, EEPROM), SRAM,
  the ISViewer debug port, a high-level boot.
- An RSP interpreter that runs the games' own microcode, and a software RDP: Super Mario 64, Mario Kart 64, Ocarina of
  Time, Star Fox 64, Paper Mario, Super Smash Bros. and Wave Race 64 render their title and opening screens on the PC.
- On the 2DS (`core/host3ds`): the same path renders Super Mario 64's title on the device, slowly (~4%). The fast path
  (high-level microcode, the RDP on the 3DS GPU) is next (`docs/DESIGN.md`). With the RSP stubbed, the JIT runs the
  games' CPU side at ~30% (`docs/research/MEASUREMENTS.md`). Remote runs go through gc3ds's relay
  (`tools/relay.sh`).

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
