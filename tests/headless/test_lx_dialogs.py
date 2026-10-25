#!/usr/bin/env python3
"""Win32 dialog boxes (DialogBoxIndirectParam, both template formats): WM_INITDIALOG, Tab / space / arrow / Enter / Escape.

    sh tools/lx/build_tests.sh && python3 tests/headless/test_lx_dialogs.py
"""
import sys, os, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("OKVM_ISO", os.path.join(HERE, "..", "..", "build-host", "lxtest.iso"))
from okvm import OkVM

fails = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails.append(what)

vm = OkVM("dialogs")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "desktop up")
    time.sleep(8)
    vm.type_string("lx /win/dlgdemo.exe\n")
    check(vm.wait_for("dialog ready", timeout=60), "the dialog is created and WM_INITDIALOG ran (old DLGTEMPLATE format)")
    time.sleep(1.5)
    vm.type_string("world"); time.sleep(0.3)                         # the edit control has the focus, its text is selected: typing replaces it
    vm.mon("sendkey tab", 0.4)                                        # -> check box
    vm.mon("sendkey spc", 0.4)                                        # toggles it
    vm.mon("sendkey tab", 0.4)                                        # -> first radio button
    vm.mon("sendkey down", 0.4)                                       # arrow: second radio button, checked
    vm.mon("sendkey ret", 0.5)                                        # the default button
    check(vm.wait_for("dialog ok name=world check=1 radio=104", timeout=15), "typing, Tab, space, arrow and Enter (default button) give the right control states")
    check(vm.wait_for("first dialog result 1", timeout=15), "EndDialog returns its result from DialogBoxIndirectParam")
    check(vm.wait_for("dialog ready", timeout=20) or vm.serial().count("dialog ready") >= 2, "the DLGTEMPLATEEX format works too")
    time.sleep(1.5)
    vm.mon("sendkey esc", 0.5)
    check(vm.wait_for("dialog cancelled", timeout=15), "Escape sends IDCANCEL")
    check(vm.wait_for("second dialog result 0", timeout=15), "the second dialog returns 0")
    check(vm.wait_for("dlgdemo done", timeout=15), "the program ends")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
