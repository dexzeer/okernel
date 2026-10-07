#!/usr/bin/env python3
"""Verify the HTTPS lock icon click opens (and a second click closes) the
security popup card.

Loads https://example.com/ (lock green/secure), closed-loop clicks the lock
icon via [mse] presses, and checks the kernel's `[okai] security popup
open/closed` lines plus the card pixels: a white 300x150 card anchored under
the address bar at (172, 101) over the page's #eee background.

Geometry (okai maximized at (0,0), border 2, no title bar): address bar from
x = 172 in the toolbar row y 40..98; lock hit box x 174..194, y 48..68.
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR
from PIL import Image

def card_white(path):
    im = Image.open(path).convert("RGB")
    px = im.load()
    return sum(1 for y in range(110, 245, 2) for x in range(180, 465, 2) if px[x, y] == (255, 255, 255))

vm = OkVM("lock")
vm.wait_for("[mem] heap", timeout=40)
time.sleep(10)
vm.type_string("okai https://example.com/\n")
loaded = vm.wait_for("https parse: count=", timeout=90)
time.sleep(2)

opened = vm.click_screen(184, 58, r"security popup open")
time.sleep(1.5)
a = os.path.join(OUTDIR, "lock_a.ppm")
vm.dump(a)
wa = card_white(a)

closed = vm.click_screen(184, 58, r"security popup closed")
time.sleep(1.5)
b = os.path.join(OUTDIR, "lock_b.ppm")
vm.dump(b)
wb = card_white(b)
vm.kill()

print(f"loaded={loaded} opened={opened} card_white={wa} closed={closed} after={wb}")
ok = loaded and opened and wa > 1500 and closed and wb < 200
print("LOCK_TEST", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
