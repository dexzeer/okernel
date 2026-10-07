#!/usr/bin/env python3
"""Regression test for the tabbed okai:
1. bare `okai` opens the internal homepage (no network)
2. clicking a link navigates the CURRENT tab in place (no new window)
3. '+' opens a new tab on the homepage; clicking a tab switches
4. chrome never bleeds into the page area (z-order sanity)
Ground truth: [mse] / [okai] click positions + [okai]/[br] serial lines.

Chrome geometry (okai opens maximized at (0,0), border 2, no title bar):
tab strip y 2..40, tab i at x = 6 + 302*i (300 wide), '+' box right after
the last tab (32px), page area from y = 98.
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

vm = OkVM("links")
vm.wait_for("[mem] heap", timeout=40)
time.sleep(10)
vm.type_string("okai\n")
ok_home = vm.wait_for("home rendered", timeout=30)
time.sleep(2)
print("HOMEPAGE:", "PASS" if ok_home else "FAIL", flush=True)

hit = vm.click_link("example.com")
inplace = hit and vm.wait_for("parse: count=", timeout=90)
inplace = inplace and "switch tab" not in vm.serial()
print("LINK IN-PLACE:", "PASS" if inplace else "FAIL", flush=True)
time.sleep(2)

# '+' after the single tab -> a new tab on the homepage
homes = vm.serial().count("home rendered")
newtab = vm.click_screen(6 + 302 + 16, 21, r"nav action=5")
newtab = newtab and vm.serial().count("home rendered") > homes
print("NEWTAB->HOME:", "PASS" if newtab else "FAIL", flush=True)
time.sleep(1)

# click tab 0 -> switch back to example.com (no refetch)
parses = vm.serial().count("parse: count=")
switched = vm.click_screen(6 + 120, 21, r"switch tab -> 0")
switched = switched and vm.serial().count("parse: count=") == parses
print("TAB SWITCH:", "PASS" if switched else "FAIL", flush=True)
time.sleep(2)

# z-order sanity: example.com's page band right under the chrome must be its
# flat background (#eee) — no chrome pixels bleeding down — while the tab
# strip itself is not page-colored.
vm.dump()
from PIL import Image
im = Image.open(vm.PPM).convert('RGB')
bg = (238, 238, 238)
page = [im.getpixel((x, y)) for y in range(100, 140, 3) for x in range(40, 1860, 9)]
leak = sum(1 for p in page if p != bg)
chrome = sum(1 for x in range(700, 1700, 10) if im.getpixel((x, 20)) != bg)
bounds_ok = leak == 0 and chrome > 50
print(f"CHROME-BOUNDS: chrome={chrome} leak={leak}", "PASS" if bounds_ok else "FAIL", flush=True)

vm.kill()
allok = ok_home and inplace and newtab and switched and bounds_ok
print("LINKS_TEST", "PASS" if allok else "FAIL")
sys.exit(0 if allok else 1)
