#!/usr/bin/env python3
"""Capture the desktop (with the main Terminal window) to verify the OS-wide
Firefox-blue theming — no browser window opened, so the title bar / taskbar
chrome is what we inspect. Writes ~/okvm/desktop.png."""
import sys, os, time, zlib, struct, binascii
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

def ppm_to_png(ppm, png):
    with open(ppm, 'rb') as f: data = f.read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    raw = parts[3][:w * h * 3]
    def chunk(typ, payload):
        return (struct.pack(">I", len(payload)) + typ + payload +
                struct.pack(">I", binascii.crc32(typ + payload) & 0xffffffff))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    stride = w * 3
    rows = b''
    for y in range(h):
        rows += b'\x00' + raw[y * stride:(y + 1) * stride]
    idat = zlib.compress(rows, 9)
    out = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
           chunk(b'IDAT', idat) + chunk(b'IEND', b''))
    with open(png, 'wb') as f: f.write(out)

vm = OkVM("desktop")
time.sleep(16)  # let desktop + main terminal settle
w, h, _ = vm.dump()
png = os.path.expanduser("~/okvm/desktop.png")
ppm_to_png(vm.PPM, png)
print(f"screenshot: {png} ({w}x{h})")
vm.kill()
