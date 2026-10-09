#!/usr/bin/env python3
"""Boot with a virtio-gpu display, optionally type commands, write ~/okvm/<tag>.png
(and print how many distinct colours the screen has).

    python3 tests/headless/shot_virtio.py [tag] [cmd ...]
"""
import sys, os, time, zlib, struct, binascii
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("OKVM_VGA", "virtio")
from okvm import OkVM

def ppm_to_png(ppm, png):
    data = open(ppm, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    raw = parts[3][:w * h * 3]
    def chunk(typ, payload):
        return struct.pack(">I", len(payload)) + typ + payload + struct.pack(">I", binascii.crc32(typ + payload) & 0xffffffff)
    rows = b''.join(b'\x00' + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))
    open(png, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                          chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))

tag = sys.argv[1] if len(sys.argv) > 1 else "virtio_shot"
vm = OkVM(tag)
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(10)
    for c in sys.argv[2:]:
        vm.type_string(c + "\n", delay=0.03)
        time.sleep(2)
    w, h, raw = vm.dump(os.path.expanduser(f"~/okvm/{tag}.ppm"))
    png = os.path.expanduser(f"~/okvm/{tag}.png")
    ppm_to_png(os.path.expanduser(f"~/okvm/{tag}.ppm"), png)
    colors = len(set(raw[i:i + 3] for i in range(0, min(len(raw), w * h * 3), 3 * 97)))
    print(f"{w}x{h} sampled colours={colors} -> {png}")
finally:
    vm.kill()
