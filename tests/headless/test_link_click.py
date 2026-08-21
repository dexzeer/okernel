#!/usr/bin/env python3
"""Headless regression: clicking a blue link must open a new okai window that
actually fetches and renders the page (regression for the black-window bug,
HANDOFF.md bug #21/#22). Verdict = serial-log lifecycle chain."""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

vm = OkVM("lc")
time.sleep(14)

# 1. Load a page with a link
vm.type_string("okai https://example.com/\n")
ok1 = vm.wait_for("https parse: count=", timeout=70, fail_pats=["FAILED"])
print("page1 loaded:", ok1)
if not ok1:
    vm.kill(); sys.exit(1)
time.sleep(2)

# 2. Closed-loop click on the first link (ground truth = serial lines)
links = vm.link_regions()
if not links:
    print("FAIL: no link regions logged"); vm.kill(); sys.exit(1)
_, row, col0, col1, href = links[0]
print(f"clicking link at row={row} cols {col0}..{col1} -> {href}")
hit = vm.click_link(row, col0, col1)
print("LINK HIT:", hit)

# 3. Watch the new window's full fetch lifecycle in serial.
# Count-based wait: "parse: count=" is ALREADY in the log from page 1,
# so a substring wait_for races and returns instantly (n1 == n0).
n0 = len(re.findall(r"\[br\] (?:https )?parse: count=\d+", vm.serial()))
parsed2 = False
t0 = time.time()
while time.time() - t0 < 120:
    n1 = len(re.findall(r"\[br\] (?:https )?parse: count=\d+", vm.serial()))
    if n1 > n0:
        parsed2 = True
        break
    time.sleep(1)
n1 = len(re.findall(r"\[br\] (?:https )?parse: count=\d+", vm.serial()))
vm.kill()

log = vm.serial()  # serial file survives kill
chain_ok = all(p in log for p in
               ["[okai] LINK HIT", "[okai] opened:", "DNS resolved"])
print("lifecycle chain complete:", chain_ok)
print("parses before/after:", n0, n1)
verdict = hit and chain_ok and n1 > n0
print("\nVERDICT:", "PASS" if verdict else "FAIL")
sys.exit(0 if verdict else 1)
