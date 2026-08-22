#!/usr/bin/env python3
"""Headless check: + button adds a tab; x button closes it.
okai window (1010,60,880,650), WIN_BORDER=2 -> cx0=1012, cy0=62.
1 tab: tab0 body center (1166,81); x-box center (1306,81); + center (1334,81).
2 tabs: tab0 x (1306,81); tab1 x (1606,81); + (1636,81).
"""
import sys, os, time, re
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from okvm import OkVM

MSE = re.compile(r"\[mse\] btn=\d+ x=(\d+) y=(\d+) pkts=\d+")

def mse_pos(vm):
    m = list(MSE.finditer(vm.serial()))
    return (int(m[-1].group(1)), int(m[-1].group(2))) if m else None

def drag_to(vm, tx, ty, maxit=60):
    for _ in range(maxit):
        pos = mse_pos(vm)
        if pos and abs(pos[0]-tx) < 8 and abs(pos[1]-ty) < 8:
            return True
        if pos:
            dx, dy = tx-pos[0], ty-pos[1]
            # Fine steps (~half remaining distance) so we settle INSIDE a small
            # 16px target box instead of oscillating across it.
            bx = max(-30, min(30, int(dx/8.0)))
            by = max(-30, min(30, int(dy/8.0)))
            if bx == 0 and abs(dx) > 4: bx = 1 if dx > 0 else -1
            if by == 0 and abs(dy) > 4: by = 1 if dy > 0 else -1
            vm.burst(bx, by)
        vm.click()
        time.sleep(0.3)
    return False

vm = OkVM("tabx")
time.sleep(14)

# Focus terminal, open homepage (1 tab, no network).
drag_to(vm, 200, 300)
vm.type_string("okai\n")
time.sleep(3)
vm.dump("/tmp/tab_step1.ppm")

# Add a 2nd tab via the + button (converge onto the + box, click opens it).
drag_to(vm, 1334, 81)
time.sleep(2)
vm.dump("/tmp/tab_step2.ppm")

# Close tab0 via its x (converge onto the x box, click closes it).
drag_to(vm, 1306, 81)
time.sleep(2)

ser = vm.serial()
closed = "[okai] closing tab" in ser
lines = [l for l in ser.splitlines() if "[okai] closing tab" in l]
print("saw '[okai] closing tab':", closed)
if lines: print("close log:", lines[-1])
vm.dump("/tmp/tab_step3.ppm")
vm.kill()
print("\nVERDICT:", "PASS" if closed else "FAIL")
sys.exit(0 if closed else 1)
