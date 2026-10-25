"""peterlemon.py ROOT [NAME_FILTER]: runs PeterLemon's N64 test ROMs (github.com/PeterLemon/N64, Unlicense; fetched into
third_party/testroms, never committed) and compares each one's last VI picture with the reference PNG beside it.

A test prints its results as text into the framebuffer with the CPU (PASS in green, FAIL in red), so the picture is the
result. Reported per ROM: pixels differing from the reference, and red (FAIL-coloured) pixels in ours and in theirs.
Standard library only (zlib for the PNGs).
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
EMU = os.path.join(HERE, "..", "build", "n64emu")


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, w = 8, b"", 0
    plte = None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and interlace == 0, (path, depth, interlace)
        elif kind == b"PLTE":
            plte = body
        elif kind == b"IDAT":
            idat += body
    bpp = {2: 3, 6: 4, 0: 1, 3: 1, 4: 2}[ctype]
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    px = []
    for row in rows:
        for x in range(w):
            if ctype == 2: px.append(tuple(row[x * 3:x * 3 + 3]))
            elif ctype == 6: px.append(tuple(row[x * 4:x * 4 + 3]))
            elif ctype == 3: i = row[x]; px.append(tuple(plte[i * 3:i * 3 + 3]))
            else: v = row[x * bpp]; px.append((v, v, v))
    return w, h, px


def read_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    body = parts[3]
    return w, h, [tuple(body[i:i + 3]) for i in range(0, w * h * 3, 3)]


def write_png(path, w, h, px):
    raw = b"".join(b"\0" + bytes(c for p in px[y * w:(y + 1) * w] for c in p) for y in range(h))
    chunk = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def red(px):
    return sum(1 for r, g, b in px if r > 160 and g < 80 and b < 80)


def run(rom, fields):
    with tempfile.TemporaryDirectory() as d:
        subprocess.run([EMU, rom, "--fields", str(fields), "--out", d], capture_output=True, timeout=300)
        p = os.path.join(d, "last.ppm")
        return read_ppm(p) if os.path.exists(p) else None


def main():
    root = sys.argv[1]
    filt = sys.argv[2] if len(sys.argv) > 2 else ""
    fields = int(os.environ.get("FIELDS", "120"))
    total = same = 0
    for dirpath, _, files in sorted(os.walk(root)):
        for f in sorted(files):
            if not f.upper().endswith(".N64") or filt not in os.path.join(dirpath, f):
                continue
            rom = os.path.join(dirpath, f)
            refs = [x for x in files if x.lower().endswith(".png") and not x.startswith("Font")]
            ours = run(rom, fields)
            total += 1
            name = os.path.relpath(rom, root)
            if not ours:
                print("%-60s no picture" % name)
                continue
            if os.environ.get("SAVE"):  # SAVE=DIR: our pictures as PNG, to look at beside the references
                os.makedirs(os.environ["SAVE"], exist_ok=True)
                write_png(os.path.join(os.environ["SAVE"], f.rsplit(".", 1)[0] + ".png"), *ours)
            if not refs:
                print("%-60s red %d (no reference)" % (name, red(ours[2])))
                continue
            best = None
            for r in refs:
                rw, rh, rpx = read_png(os.path.join(dirpath, r))
                ow, oh, opx = ours
                if rw == ow and rh >= 2 * oh - 6 and oh < rh:  # a field-height picture against a frame capture: double
                    opx = [p for y in range(oh) for _ in range(2) for p in opx[y * ow:(y + 1) * ow]]
                    oh *= 2
                    ours = (ow, oh, opx)
                if rw != ours[0]:
                    diff = None
                else:
                    # The rows both pictures have (n64emu's picture height from the VI registers is approximate)
                    n = min(rh, ours[1]) * rw
                    diff = sum(1 for a, b in zip(ours[2][:n], rpx[:n]) if a != b)
                if best is None or (diff is not None and (best[0] is None or diff < best[0])):
                    best = (diff, r, red(rpx), rw, rh)
            diff, r, rred, rw, rh = best
            if diff == 0:
                same += 1
            size = "" if diff is not None else " size %dx%d vs %dx%d" % (ours[0], ours[1], rw, rh)
            print("%-60s diff %s red %d (reference %d)%s" % (name, diff, red(ours[2]), rred, size))
    print("%d ROMs, %d identical to the reference" % (total, same))


if __name__ == "__main__":
    main()
