#!/usr/bin/env python3
"""Headless check: the '+' button adds a tab; a tab's x box closes it.

Positions come from okai's `[okai] ui` geometry log (newtab, tabx<i>).
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

added = vm.click_ui("newtab", r"nav action=5")
time.sleep(1)
added = added and vm.serial().count("home rendered") >= 2
print("'+' adds a tab:", added, flush=True)

for _ in range(20):  # the 2nd tab's geometry is logged once its open animation settles
    if vm.ui_rect("tabx1"): break
    time.sleep(0.5)
closed = vm.click_ui("tabx1", r"\[okai\] closing tab 1")
time.sleep(1)
print("saw '[okai] closing tab':", closed, flush=True)

vm.kill()
ok = ok_home and added and closed
print("\nVERDICT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
