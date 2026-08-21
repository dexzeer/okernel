#!/usr/bin/env python3
"""Headless regression: google.com must render cleanly (regression for the
garbled-text bug, HANDOFF.md bug #23). Checks: full chunked body received,
redirect followed, many tokens parsed, page background applied (white, not
the default black). Requires internet access for SLIRP forwarding."""
import sys, os, time, re, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

vm = OkVM("gg")
time.sleep(14)

vm.type_string("okai http://google.com/\n")

# google.com 301s to www.google.com; both fetches must complete
parsed = vm.wait_for("parse: count=", timeout=150,
                     fail_pats=["FAILED", "giving up", "timed out"])
log = vm.serial()

m = re.search(r"\[br\] parse: count=(\d+) len=(\d+)", log)
count = int(m.group(1)) if m else 0
length = int(m.group(2)) if m else 0
redirect = "redirect 1 -> http://www.google.com/" in log
print(f"parsed: {parsed}, tokens: {count}, body bytes: {length}, redirect followed: {redirect}")

# Pixel check: okai window content area (window at 30,20 + border 2 + title 24
# => content x=32..548, y=46..416). google's body{background:#fff} must win
# over the default black — count pixels.
time.sleep(2)
w, h, px = vm.dump()
vm.kill()
cnt = collections.Counter()
for y in range(46, 416):
    base = y * w
    for x in range(32, 548):
        i = (base + x) * 3
        cnt[px[i:i+3]] += 1
top = cnt.most_common(4)
print("content-area top colors:", [(c.hex(), n) for c, n in top])
white_bg = top[0][0] == b'\xff\xff\xff'
blue_links = cnt.get(b'\x55\x55\xff', 0) > 100   # VGA 9 = 85,85,255

verdict = parsed and count >= 40 and length > 80000 and redirect and white_bg
print("white background applied:", white_bg, "| blue links present:", blue_links)
print("\nVERDICT:", "PASS" if verdict else "FAIL")
sys.exit(0 if verdict else 1)
