#!/usr/bin/env python3
"""Verify the HTTPS lock icon click opens (and a second click closes) the
security popup card.

Loads https://example.com/ (secure), closed-loop clicks the site-identity
(lock) box in the address bar via [mse] presses, and checks the kernel's
`[okai] security popup open/closed` lines plus the card pixels: a white
380x168 panel anchored under the identity box over the page's #eee
background. Positions come from okai's `[okai] ui` geometry log.
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR
from PIL import Image

def card_white(path, ident, url):
    im = Image.open(path).convert("RGB")
    px = im.load()
    y0 = url[1] + url[3] + 16
    return sum(1 for y in range(y0, y0 + 140, 2) for x in range(ident[0] + 10, ident[0] + 360, 2)
               if px[x, y] == (255, 255, 255))

vm = OkVM("lock")
vm.wait_for("[mem] heap", timeout=40)
time.sleep(10)
vm.type_string("okai https://example.com/\n")
loaded = vm.wait_for("https parse: count=", timeout=90)
time.sleep(2)

ident, url = vm.ui_rect("ident"), vm.ui_rect("url")
opened = vm.click_ui("ident", r"security popup open")
time.sleep(1.5)
a = os.path.join(OUTDIR, "lock_a.ppm")
vm.dump(a)
wa = card_white(a, ident, url)

closed = vm.click_ui("ident", r"security popup closed")
time.sleep(1.5)
b = os.path.join(OUTDIR, "lock_b.ppm")
vm.dump(b)
wb = card_white(b, ident, url)
vm.kill()

print(f"loaded={loaded} opened={opened} card_white={wa} closed={closed} after={wb}")
ok = loaded and opened and wa > 1500 and closed and wb < 200
print("LOCK_TEST", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
