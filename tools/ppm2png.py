#!/usr/bin/env python3
"""ppm2png.py in.ppm out.png [x y w h] -- convert (and optionally crop) a QEMU screendump to PNG."""
import sys, re, zlib, struct
d = open(sys.argv[1], "rb").read()
m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", d)
w, h = int(m.group(1)), int(m.group(2)); body = d[m.end():]
x0, y0, cw, ch = 0, 0, w, h
if len(sys.argv) >= 7: x0, y0, cw, ch = map(int, sys.argv[3:7])
rows = b"".join(b"\0" + body[((y0 + y) * w + x0) * 3:((y0 + y) * w + x0 + cw) * 3] for y in range(ch))
def chunk(t, c): return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", cw, ch, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
open(sys.argv[2], "wb").write(png)
