#!/usr/bin/env python3
"""The normal desktop image (kanarchy-desktop.iso, no test ISO) carries the Linux/Windows subsystem: a Linux program, a
Windows program and the X server with a client run from it.

    make desktop && python3 tests/headless/test_lx_image.py
"""
import sys, os, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("OKVM_ISO", os.path.join(HERE, "..", "..", "kanarchy-desktop.iso"))
from okvm import OkVM

fails = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails.append(what)

vm = OkVM("lximage")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "the subsystem is up in the normal image")
    time.sleep(8)
    vm.type_string("lx /bin/hello\n")
    check(vm.wait_for("hello from x86-64 Linux", timeout=40), "a Linux program runs")
    time.sleep(1)
    vm.type_string("lx /win/hello.exe\n")
    check(vm.wait_for("hello from a Windows program", timeout=40), "a Windows program runs")
    time.sleep(3)
    vm.type_string("lx /bin/xokai &\n")
    check(vm.wait_for("xokai: display :0 ready", timeout=40), "the X server starts")
    time.sleep(1)
    vm.type_string("lx /bin/xtest\n")
    check(vm.wait_for("xtest: drawn", timeout=60), "an X client draws")
    vm.mon("sendkey esc", 0.5)
    check(vm.wait_for("xtest: bye", timeout=20), "and ends")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
