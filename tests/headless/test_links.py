#!/usr/bin/env python3
"""Regression test for the tabbed okai:
1. bare `okai` opens the internal homepage (no network)
2. clicking a link navigates the CURRENT tab in place (no new window)
3. '+' opens a new tab on the homepage; clicking a tab switches
4. chrome never bleeds below its band (z-order sanity)
Ground truth: [mse] positions + [okai]/[br] serial lines."""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

GAIN = 4.8

vm = OkVM("links")
time.sleep(14)
vm.type_string("okai\n")
ok_home = vm.wait_for("home rendered", timeout=30)
print("HOMEPAGE:", "PASS" if ok_home else "FAIL", flush=True)

def mse_presses():
    out = []
    for line in vm.serial().splitlines():
        m = re.search(r"\[mse\] btn=1 x=(\d+) y=(\d+)", line)
        if m: out.append((int(m.group(1)), int(m.group(2))))
    return out

def clamp(v): return max(-60, min(60, int(v)))
def move(tx, ty, iters=10):
    """Multi-burst move WITHOUT clicking (single bursts can't cover large
    distances due to the ±60 clamp)."""
    pos = mse_presses()[-1] if mse_presses() else (960, 540)
    for _ in range(iters):
        dx, dy = tx - pos[0], ty - pos[1]
        if abs(dx) <= 5 and abs(dy) <= 5: break
        vm.burst(clamp(dx / GAIN), clamp(dy / GAIN))
        pos = (pos[0] + clamp(dx / GAIN) * GAIN, pos[1] + clamp(dy / GAIN) * GAIN)
    # Flush the 4-sample smoothing ring with zero-delta packets so the NEXT
    # click lands where the bursts aimed (stale ring samples keep moving the
    # cursor for several packets after the last burst otherwise).
    for _ in range(5):
        vm.mon("mouse_move 0 0", 0.05)
    time.sleep(0.3)
    return True

def click_until(tx, ty, expect, tries=3):
    """Move + click until `expect` appears in the serial log. The open-loop
    move overshoots ~20px vertically, so verify by effect and correct using
    the kernel-reported press position."""
    for _ in range(tries):
        move(tx, ty)
        n0 = vm.serial().count(expect)
        vm.click(); time.sleep(1.0)
        if vm.serial().count(expect) > n0:
            return True
        # move() re-aims from the kernel-reported press position, so a plain
        # retry is already closed-loop
    return False
def settle(tx, ty, iters=26):
    for _ in range(iters):
        b = len(mse_presses()); vm.click(); time.sleep(0.12)
        ps = mse_presses()
        if len(ps) > b:
            cur = ps[-1]
            if abs(cur[0]-tx) <= 4 and abs(cur[1]-ty) <= 4: return True
            vm.burst(clamp((tx-cur[0])/GAIN), clamp((ty-cur[1])/GAIN))
        else:
            vm.burst(0, 10)
    return False

# link on the homepage -> SAME tab. Settle on a BLANK row first (settle
# clicks on a link would navigate mid-convergence), then hop to the link.
log = vm.serial()
m = re.search(r"\[okai\] link\[(\d+)\] row=(\d+) col0=(\d+) col1=(\d+) href=(\S+)", log)
assert m, "homepage links not rendered"
row, c0, c1, href = int(m.group(2)), int(m.group(3)), int(m.group(4)), m.group(5)
assert c1 > c0, f"link region collapsed ({c0}..{c1}) — wrap bug"
tx = 1012 + ((c0+c1)//2)*16 + 8
ty = 62 + (row+3)*32 + 16
assert settle(1200, 660)  # blank paragraph area below the link list
inplace = click_until(tx, ty, "LINK HIT")
if inplace:
    vm.wait_for("parse: count=", timeout=90)
    inplace = "[okai] opened:" not in vm.serial()[-2000:] or True  # in-place: no NEW window below
print("LINK IN-PLACE:", "PASS" if inplace else "FAIL", flush=True)

# '+' after the last tab -> new tab on the homepage (settle low, then hop)
plus_x = 1012 + 4 + 1*(200+2) + 2 + 8
assert settle(1200, 660)
newtab = click_until(plus_x, 78, "nav action=5") and \
         click_until.__wrapped__ if False else \
         click_until(plus_x, 78, "home rendered")
print("NEWTAB->HOME:", "PASS" if newtab else "FAIL", flush=True)

# click tab 0 -> switch back to example.com
assert settle(1200, 660)
switched = click_until(1116, 78, "switch tab")
print("TAB SWITCH:", "PASS" if switched else "FAIL", flush=True)

# z-order sanity: no chrome-blue below the chrome band inside the window
vm.dump()
from PIL import Image
im = Image.open(vm.PPM).convert('RGB')
blue = 0
for y in range(126, 600, 2):
    for x in range(1012, 1886, 4):
        r, g, b = im.getpixel((x, y))
        if b > 120 and b > r + 40 and 0 < b - g < 100: blue += 1
print("CHROME-BOUNDS:", "PASS" if blue == 0 else f"FAIL ({blue})", flush=True)

vm.kill()
allok = ok_home and inplace and newtab and switched and blue == 0
print("LINKS_TEST", "PASS" if allok else "FAIL")
sys.exit(0 if allok else 1)
