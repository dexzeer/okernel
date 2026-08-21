#!/usr/bin/env python3
"""Headless test for the browser toolbar nav buttons (back/fwd/reload/home).
Convergence uses the kernel's [mse] btn=1 serial lines (logged on EVERY button
press, anywhere on screen) — not [okai] click lines, which only appear inside
browser content and leave the loop blind once the cursor drifts outside the
window. For each button column x, settle in deep content, then sweep UP through
the toolbar band clicking; the click that lands on the button logs the matching
nav action=N."""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

GAIN = 4.8  # measured mid-flight (~5.0 px/unit; probe quiescent ~4.4)
# Button x-centers (window 1010,60, no_titlebar). Geometry mirrors the draw:
# cx0=1012 btn=30 gap=8 -> centers 1035/1073/1111/1149, y band 87..117.
TARGETS = [
    (1, 1035),  # NAV_BACK
    (2, 1073),  # NAV_FWD
    (3, 1111),  # NAV_RELOAD
    (4, 1149),  # NAV_HOME
]
CONTENT_Y = 300

vm = OkVM("nav")
time.sleep(14)
vm.type_string("okai http://example.com/\n")
vm.wait_for("parse: count=", timeout=90)
time.sleep(2)

def nav_lines():
    return [int(m.group(1)) for m in re.finditer(r"\[okai\] nav action=(\d+)", vm.serial())]

def mse_presses():
    """(x, y) of every mouse button press, from the kernel's [mse] lines."""
    out = []
    for line in vm.serial().splitlines():
        m = re.search(r"\[mse\] btn=1 x=(\d+) y=(\d+)", line)
        if m: out.append((int(m.group(1)), int(m.group(2))))
    return out

def clamp(v): return max(-60, min(60, int(v)))
def br(dx): return clamp(dx / GAIN)

def settle_to(tx, ty, max_iters=22):
    for _ in range(max_iters):
        b = len(mse_presses()); vm.click(); time.sleep(0.15)
        ps = mse_presses()
        if len(ps) > b:
            cur = ps[-1]
            if abs(cur[0]-tx) <= 4 and abs(cur[1]-ty) <= 4:
                return True
            vm.burst(br(tx-cur[0]), br(ty-cur[1]))
        else:
            vm.burst(0, 10)
    return False

results = []
for expected, tx in TARGETS:
    settle_to(tx, CONTENT_Y)
    before = len(nav_lines())
    seen = []
    for _ in range(22):
        vm.burst(0, -3); time.sleep(0.06)
        vm.click(); time.sleep(0.1)
        nl = nav_lines()
        if len(nl) > before:
            for a in nl[before:]:
                if a not in seen: seen.append(a)
            before = len(nl)
    ok = expected in seen
    results.append((expected, seen, ok))
    print(f"expected nav={expected} seen={seen} {'OK' if ok else 'FAIL'} (x={tx})", flush=True)
    time.sleep(0.3)

all_ok = all(ok for _, _, ok in results)
print("NAV_TEST", "PASS" if all_ok else "FAIL", results)
vm.kill()
sys.exit(0 if all_ok else 1)
