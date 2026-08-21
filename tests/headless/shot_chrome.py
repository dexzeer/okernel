#!/usr/bin/env python3
"""Headless visual check for the new pixel chrome: boot, open example.com,
screendump to PPM, convert to PNG at /tmp/ok/chrome.png for inspection.
Requires internet (SLIRP) for a real page; the chrome renders regardless.
"""
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

vm = OkVM("chrome")
time.sleep(14)
vm.type_string("okai http://example.com/\n")
vm.wait_for("parse: count=", timeout=120)
time.sleep(3)
w, h, _ = vm.dump()
png = os.path.expanduser("~/okvm/chrome.png")
ppm_to_png(vm.PPM, png)
print(f"screenshot: {png} ({w}x{h})")
vm.kill()
