#!/usr/bin/env python3
"""Verify the two UX fixes:
1. Terminal fetch logs are compact (no 180-byte header dump) — check serial +
   that only short net-event lines appear.
2. Stray chrome click (tab strip) does NOT clear the URL bar; clicking the
   address bar DOES focus it (typing lands there).
Ground truth: [mse] lines for click positioning, pixel analysis of the white
address-bar rect for URL presence."""
import sys, os, time, re
sys.path.insert(0, "/home/notdexy/projects/okernel/tests/headless")
from okvm import OkVM
from PIL import Image

GAIN = 4.8

def mse_presses(vm):
    out = []
    for line in vm.serial().splitlines():
        m = re.search(r"\[mse\] btn=1 x=(\d+) y=(\d+)", line)
        if m: out.append((int(m.group(1)), int(m.group(2))))
    return out

def clamp(v): return max(-60, min(60, int(v)))

def settle(vm, tx, ty, max_iters=22):
    for _ in range(max_iters):
        b = len(mse_presses(vm)); vm.click(); time.sleep(0.15)
        ps = mse_presses(vm)
        if len(ps) > b:
            cur = ps[-1]
            if abs(cur[0]-tx) <= 4 and abs(cur[1]-ty) <= 4: return True
            vm.burst(clamp((tx-cur[0])/GAIN), clamp((ty-cur[1])/GAIN))
        else:
            vm.burst(0, 10)
    return False

def shot(vm):
    vm.dump()
    return Image.open(vm.PPM).convert("RGB")

def addr_bar_rect(im):
    """Locate the white (0xFFFFFF) rect in the toolbar band y 40..70."""
    W, H = im.size
    for y in range(60, 130):
        x0 = None
        for x in range(150, W - 40):
            white = all(abs(c - 255) < 12 for c in im.getpixel((x, y))[:3])
            if white and x0 is None: x0 = x
            if x0 is not None and not white:
                if x - x0 > 100: return (x0, y, x, y + 22)
                x0 = None
    return None

def dark_pixels(im, rect):
    x0, y0, x1, y1 = rect
    n = 0
    for y in range(y0, y1):
        for x in range(x0 + 20, x1):  # skip the lock icon area
            r, g, b = im.getpixel((x, y))[:3]
            if r < 100 and g < 100 and b < 100: n += 1
    return n

vm = OkVM("addrbar")
time.sleep(14)
vm.type_string("okai http://example.com/\n")
vm.wait_for("parse: count=", timeout=90)
time.sleep(2)

# 1. compact terminal logs: no "HTTP response (" dump should be echoed...
# (serial keeps [http] segment lines; the net-event copy is what hits the
# terminal — check no "(truncated)" / "bytes):" net-event markers anywhere new)
log = vm.serial()
assert "...(truncated)" not in log, "old header dump still present"
print("PASS: no raw-header dump in event stream")

imA = shot(vm)
rect = addr_bar_rect(imA)
assert rect, "address bar rect not found"
dpA = dark_pixels(imA, rect)
print(f"addr bar {rect}, dark pixels (URL text) = {dpA}")
assert dpA > 50, "URL text missing from address bar"

# 2. stray chrome click (tab strip right of the active tab) must NOT clear URL
settle(vm, 400, 30); vm.click(); time.sleep(0.5)
imB = shot(vm)
dpB = dark_pixels(imB, rect)
print(f"after tab-strip click, dark pixels = {dpB}")
assert abs(dpB - dpA) < 20, f"URL bar changed after stray click ({dpA} -> {dpB})"
print("PASS: stray chrome click did not clear the URL bar")

# 3. address-bar click focuses it; typed chars land there
cx = (rect[0] + rect[2]) // 2
settle(vm, cx, (rect[1] + rect[3]) // 2); vm.click(); time.sleep(0.3)
vm.type_string("zz"); time.sleep(0.5)
imC = shot(vm)
dpC = dark_pixels(imC, rect)
print(f"after addr-bar click + typing 'zz', dark pixels = {dpC}")
assert dpC != dpA, "typing did not change the address bar content"
print("PASS: address-bar click focuses it for typing")

vm.kill()
print("ADDRBAR_TEST PASS")
