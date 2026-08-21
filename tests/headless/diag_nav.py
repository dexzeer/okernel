#!/usr/bin/env python3
"""Measure true toolbar button centers: settle in deep content, hop up into the
band once, then sweep x rightward clicking; each nav hit logs (action,mx,my),
revealing exactly where BACK/FWD/RELOAD/HOME sit."""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

vm = OkVM("diag")
time.sleep(14)
vm.type_string("okai http://example.com/\n")
vm.wait_for("parse: count=", timeout=90)
time.sleep(2)

def nav_tuples():
    out = []
    for line in vm.serial().splitlines():
        m = re.search(r"\[okai\] nav action=(\d+) \(mx=(\d+) my=(\d+)\)", line)
        if m: out.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    return out

def click_lines():
    out = []
    for line in vm.serial().splitlines():
        m = re.search(r"\[okai\] click row=(\d+) col=(\d+) \(mx=(\d+) my=(\d+)\)", line)
        if m: out.append((int(m.group(3)), int(m.group(4))))
    return out

def clamp(v): return max(-60, min(60, int(v)))

# Settle to (53,200)
cur = None
for _ in range(18):
    b = len(click_lines()); vm.click(); time.sleep(0.2)
    ls = click_lines()
    if len(ls) > b:
        cur = ls[-1]
        if abs(cur[0]-53) <= 4 and abs(cur[1]-200) <= 4: break
        vm.burst(clamp((53-cur[0])/3.0), clamp((200-cur[1])/3.0))
    else:
        if cur: vm.burst(clamp((53-cur[0])/3.0), clamp((200-cur[1])/3.0))
        else: vm.burst(0, 25)

print("settled at", cur, flush=True)
# Hop up into the band (from y~200 to y~55): burst(0,-39) ~ -146px
vm.burst(0, -39); time.sleep(0.3)
before = len(nav_tuples())
# Sweep x rightward; clicks that land on buttons log (action,mx,my)
for k in range(40):
    vm.burst(8, 0)            # move right ~30px
    time.sleep(0.08)
    vm.click(); time.sleep(0.1)
# Report every distinct (action) with the set of (mx,my) seen
seen = {}
for a, mx, my in nav_tuples()[before:]:
    seen.setdefault(a, []).append((mx, my))
for a in sorted(seen):
    xs = [x for x, _ in seen[a]]
    print(f"action {a}: x range {min(xs)}..{max(xs)}  samples={seen[a][:6]}")
vm.kill()
