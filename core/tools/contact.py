"""contact.py OUT.png IMG.png ...: tiles PNG pictures (scaled to 320x240 cells, 4 per row) into one sheet."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import peterlemon as p

def main():
    out, files = sys.argv[1], sys.argv[2:]
    cw, ch, cols = 320, 240, 4
    rows = (len(files) + cols - 1) // cols
    W, H = cw * cols, ch * rows
    sheet = [(32, 32, 32)] * (W * H)
    for k, f in enumerate(files):
        w, h, px = p.read_png(f)
        ox, oy = (k % cols) * cw, (k // cols) * ch
        for y in range(ch):
            sy = y * h // ch
            for x in range(cw):
                sheet[(oy + y) * W + ox + x] = px[sy * w + x * w // cw]
    p.write_png(out, W, H, sheet)

main()
