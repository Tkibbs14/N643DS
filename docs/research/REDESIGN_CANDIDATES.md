# Redesign candidates

Every idea with redesign potential, with its evidence, cost, status (parked / next / built / rejected) and a
"re-evaluate when" trigger. Never delete an entry; update its status. Format as in gc3ds.

## U1. Word-swapped guest memory (2026-10-25)

- **Status:** built (the layout from day one).
- **Idea:** store RDRAM and the other guest memories with each 32-bit word in host order, so 32-bit guest accesses are
  plain host loads and narrow ones XOR the address (byte `^3`, halfword `^2`).
- **Evidence:** the standard choice in N64 emulators; most game accesses are 32-bit. gc3ds measured byte-swapped guest
  RAM as neutral (`GC_SWAPRAM`), but the Gekko's accesses mix widths differently.
- **Re-evaluate when:** the JIT's memory path is profiled on the console.

## U2. Graphics HLE checked against a low-level RSP (2026-10-25)

- **Status:** next (docs/DESIGN.md "Exactness model").
- **Idea:** the RSP's outputs are data (an RDP command stream, audio samples), so high-level microcode can be compared
  byte for byte with the real microcode run on a low-level RSP on the PC, as gc3ds's check modes compare ports with
  the interpreter.
- **Cost:** a low-level RSP interpreter (PC only).
- **Re-evaluate when:** a microcode's output legitimately differs only in ordering (then compare a normalised stream).
