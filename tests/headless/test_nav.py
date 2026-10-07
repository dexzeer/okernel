#!/usr/bin/env python3
"""Headless test for the browser toolbar nav buttons (back/fwd/reload/home).
Convergence uses the kernel's [mse] btn=1 serial lines (logged on every press,
anywhere on screen) via OkVM.click_screen; each button logs `[okai] nav
action=N` when hit.

Button positions come from okai's `[okai] ui` geometry log (vm.click_ui).
History is set up first (home -> example.com) so back/forward really move.
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

TARGETS = [(1, "back", "back"), (2, "fwd", "forward"), (3, "reload", "reload"), (4, "home", "home")]

vm = OkVM("nav")
vm.wait_for("[mem] heap", timeout=40)
time.sleep(10)
vm.type_string("okai\n")
vm.wait_for("home rendered", timeout=30)
time.sleep(1)
vm.type_string("g")
time.sleep(0.5)
vm.type_string("http://example.com/\n")
vm.wait_for("parse: count=", timeout=90)
time.sleep(2)

results = []
for action, ui, name in TARGETS:
    ok = vm.click_ui(ui, r"nav action=%d" % action)
    time.sleep(3)
    navs = re.findall(r"\[okai\] navigate (\S+)", vm.serial())
    last = navs[-1] if navs else ""
    if name == "back":
        ok = ok and last == "okai:home"
    elif name == "forward":
        ok = ok and "example.com" in last
    elif name == "reload":
        ok = ok and "example.com" in last
    elif name == "home":
        ok = ok and last == "okai:home"
    print(f"{name}: {'OK' if ok else 'FAIL'} (last navigate {last})", flush=True)
    results.append(ok)

vm.kill()
print("NAV_TEST", "PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)
