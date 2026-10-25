"""mkrom.py BIN OUT.z64: wraps a raw binary linked at 0x80000400 into a cartridge image for the high-level boot.

The IPL3 area (0x40-0xFFF) stays zero: n64emu does not run IPL3 (docs/DESIGN.md "Boot"), so these images boot on the
emulator only, not on hardware or in emulators that run the cartridge's IPL3.
"""
import struct
import sys


def main():
    src, dst = sys.argv[1], sys.argv[2]
    body = open(src, "rb").read()
    if len(body) > 0x100000:
        sys.exit("%s: %d bytes, more than the 1 MB the boot copies" % (src, len(body)))
    hdr = struct.pack(">IIII", 0x80371240, 0x0000000F, 0x80000400, 0x0000144C)
    hdr += struct.pack(">II", 0, 0) + b"\0" * 8
    hdr += b"N643DS CPU TEST".ljust(20, b" ")
    hdr += b"\0" * 7 + b"NUTE" + b"\0"
    assert len(hdr) == 0x40
    rom = hdr + b"\0" * (0x1000 - 0x40) + body
    rom += b"\0" * (-len(rom) % 4)
    open(dst, "wb").write(rom)


if __name__ == "__main__":
    main()
