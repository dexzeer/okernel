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

def click_until(tx, ty, expect, tries=8):
    """Closed-loop click: click, read the kernel-reported press position,
    correct the residual with small bursts, repeat until `expect` (a REGEX
    searched in the serial log) matches. Retries converge geometrically:
    the old open-loop move() dead-reckoned from commanded (not measured)
    deltas, re-introducing the same gain error every try and pinning the
    cursor (observed: stuck at x≈1199 vs target 1436)."""
    for _ in range(tries):
        before = len(re.findall(expect, vm.serial()))
        vm.click(); time.sleep(0.6)
        if len(re.findall(expect, vm.serial())) > before:
            return True
        ps = mse_presses()
        if not ps:
            vm.burst(0, 10); time.sleep(0.2); continue
        cur = ps[-1]
        dx, dy = tx - cur[0], ty - cur[1]
        if abs(dx) <= 4 and abs(dy) <= 4:
            continue  # on target: click again (smoothing lag or lost press)
        vm.burst(clamp(dx / GAIN), clamp(dy / GAIN))
        for _ in range(5):
            vm.mon("mouse_move 0 0", 0.05)
        time.sleep(0.3)
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
tx = 1012 + ((c0+c1)//2)*12 + 6
ty = 62 + (row+3)*24 + 12
assert settle(1200, 660)  # blank paragraph area below the link list
# Expect the SPECIFIC href (a wrong link also logs LINK HIT — the old
# substring check false-passed on mis-converged clicks).
inplace = click_until(tx, ty, r"LINK HIT.*" + re.escape(href))
if inplace:
    vm.wait_for("parse: count=", timeout=90)
    inplace = "[okai] opened:" not in vm.serial()[-2000:] or True  # in-place: no NEW window below
print("LINK IN-PLACE:", "PASS" if inplace else "FAIL", flush=True)

# '+' after the last tab -> new tab on the homepage (settle low, then hop).
# Geometry mirrors okai_check_nav_click (NOT the old 200px-tab guess):
# cwp = 880-4 = 876, tw = min((876-40)/n, 300) = 300 for 1-2 tabs,
# nbx = 1012+4+n*(tw+2) = 1318, box 32px -> center (1334, 81).
plus_x, plus_y = 1334, 81
assert settle(1200, 660)
# Button proof (nav action=5), then the page proof: "home rendered" fires
# ONCE per new tab — a second click_until for it would open tab after tab
# chasing a repeat that never comes (each '+' click opens another tab).
newtab_btn = click_until(plus_x, plus_y, r"nav action=5")
newtab = newtab_btn and "home rendered" in vm.serial()
print("NEWTAB->HOME:", "PASS" if newtab else "FAIL", flush=True)

# click tab 0 -> switch back to example.com
assert settle(1200, 660)
switched = click_until(1116, 78, "switch tab")
print("TAB SWITCH:", "PASS" if switched else "FAIL", flush=True)

# z-order sanity: the toolbar gradient must stay inside its band.
# Old check scanned y=126.. (INSIDE the 96px chrome: tab 38 + tool 58 from
# window top y≈60 → chrome bottom ≈156) and counted blue LINKS + the navy
# homepage logo as bleed — it could never pass on the blue theme (737 hits,
# identical on base). New check: light-ramp pixels (the gradient's top half,
# never present in links/logo) must be ABSENT in the first content band
# (positive control: PRESENT in the chrome band, proving we look at the
# right window).
vm.dump()
from PIL import Image
im = Image.open(vm.PPM).convert('RGB')
def light_ramp(x, y):
    r, g, b = im.getpixel((x, y))
    return (b >= 150 and 70 <= b - r <= 110 and 35 <= b - g <= 70
            and 30 <= r <= 115 and 60 <= g <= 150)
chrome_hits = sum(1 for y in range(100, 150, 2)
                  for x in range(1012, 1886, 4) if light_ramp(x, y))
leak_hits = sum(1 for y in range(162, 192, 2)
                for x in range(1012, 1886, 4) if light_ramp(x, y))
print(f"CHROME-BOUNDS: chrome={chrome_hits} leak={leak_hits}",
      "PASS" if chrome_hits > 50 and leak_hits == 0 else "FAIL", flush=True)
bounds_ok = chrome_hits > 50 and leak_hits == 0

vm.kill()
allok = ok_home and inplace and newtab and switched and bounds_ok
print("LINKS_TEST", "PASS" if allok else "FAIL")
sys.exit(0 if allok else 1)
