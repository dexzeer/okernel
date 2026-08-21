#!/usr/bin/env python3
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

vm = OkVM("diag")
time.sleep(14)
vm.type_string("okai http://example.com/\n")
vm.wait_for("parse: count=", timeout=90)
time.sleep(2)

def click_lines():
    out = []
    for line in vm.serial().splitlines():
        m = re.search(r"\[okai\] click row=(\d+) col=(\d+) \(mx=(\d+) my=(\d+)\)", line)
        if m: out.append((int(m.group(3)), int(m.group(4))))
    return out

def nav_tuples():
    out = []
    for line in vm.serial().splitlines():
        m = re.search(r"\[okai\] nav action=(\d+) \(mx=(\d+) my=(\d+)\)", line)
        if m: out.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    return out

def clamp(v): return max(-60, min(60, int(v)))

# Settle to (200,250) deep content, both axes, to get a known start.
cur = None
for _ in range(18):
    b = len(click_lines()); vm.click(); time.sleep(0.2)
    ls = click_lines()
    if len(ls) > b:
        cur = ls[-1]
        if abs(cur[0]-200) <= 4 and abs(cur[1]-250) <= 4: break
        vm.burst(clamp((200-cur[0])/3.0), clamp((250-cur[1])/3.0))
    else:
        if cur: vm.burst(clamp((200-cur[0])/3.0), clamp((250-cur[1])/3.0))
        else: vm.burst(0, 25)
print("settled at", cur, flush=True)

# Test HORIZONTAL movement: burst left, see if mx changes.
mx0 = cur[0]
vm.burst(-20, 0); time.sleep(0.3)
b = len(click_lines()); vm.click(); time.sleep(0.2)
ls = click_lines()
mx1 = ls[-1][0] if ls else None
print(f"horizontal burst(-20,0): mx {mx0} -> {mx1}  (delta {mx1-mx0 if mx1 else '?'})", flush=True)

# Test VERTICAL movement for comparison.
my0 = cur[1]
vm.burst(0, -20); time.sleep(0.3)
b = len(click_lines()); vm.click(); time.sleep(0.2)
ls = click_lines()
my1 = ls[-1][1] if ls else None
print(f"vertical burst(0,-20): my {my0} -> {my1}  (delta {my1-my0 if my1 else '?'})", flush=True)

# Now hop into the toolbar band and click; report any nav action.
vm.burst(0, -10); time.sleep(0.2)  # nudge up toward band
before = len(nav_tuples())
for _ in range(6):
    vm.burst(0, -3); time.sleep(0.1); vm.click(); time.sleep(0.15)
print("nav seen in band:", nav_tuples()[before:], flush=True)
vm.kill()
