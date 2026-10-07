#!/usr/bin/env python3
"""Headless check: the '+' button adds a tab; a tab's x box closes it.

Geometry (okai maximized at (0,0), border 2, no title bar): tab i spans
x = 6 + 302*i .. +300 in the strip y 2..40; its x box (16px) is centered at
(6 + 302*i + 290, 21); '+' (32px) right after the last tab, centered at
(6 + 302*n + 16, 21).
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

vm = OkVM("tabx")
vm.wait_for("[mem] heap", timeout=40)
time.sleep(10)
vm.type_string("okai\n")
ok_home = vm.wait_for("home rendered", timeout=30)
time.sleep(1)

added = vm.click_screen(6 + 302 + 16, 21, r"nav action=5")
time.sleep(1)
added = added and vm.serial().count("home rendered") >= 2
print("'+' adds a tab:", added, flush=True)

closed = vm.click_screen(6 + 302 + 290, 21, r"\[okai\] closing tab 1")
time.sleep(1)
print("saw '[okai] closing tab':", closed, flush=True)

vm.kill()
ok = ok_home and added and closed
print("\nVERDICT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
