#!/usr/bin/env python3
"""Win32 controls: COMBOBOX (drop-down list, drop-down edit), SCROLLBAR controls, the scroll bars of a window.

    sh tools/lx/build_tests.sh && python3 tests/headless/test_lx_ctls.py
"""
import sys, os, re, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("OKVM_ISO", os.path.join(HERE, "..", "..", "build-host", "lxtest.iso"))
import okvm
from okvm import OkVM

fails = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails.append(what)

def rect(log, name):
    m = re.search(name + r" (\d+),(\d+) (\d+),(\d+)", log)
    return tuple(int(v) for v in m.groups())

vm = OkVM("ctls")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "desktop up")
    time.sleep(8)
    vm.type_string("lx /win/ctldemo.exe\n")
    check(vm.wait_for("controls ready", timeout=60), "window with combo boxes and scroll bars")
    time.sleep(1.5)
    log = vm.serial()
    c1, vs = rect(log, "combo1"), rect(log, "vsb")
    cl = re.search(r"client (\d+),(\d+) (\d+),(\d+)", log); cx, cy, cw, ch = (int(v) for v in cl.groups())

    vm.click_screen((c1[0] + c1[2]) // 2, c1[1] + 12, r"dropdown")
    check(True, "clicking the combo box opens its drop-down list (CBN_DROPDOWN)")
    time.sleep(0.8)
    vm.mon("sendkey down", 0.3); vm.mon("sendkey ret", 0.5)
    check(vm.wait_for(r"selchange 201 sel=1", timeout=10), "Down + Enter in the list selects the second item (CBN_SELCHANGE, CB_GETCURSEL)")

    # scroll bar control: the down arrow, then the page area below the thumb
    vm.click_screen((vs[0] + vs[2]) // 2, vs[3] - 8, r"ctl 1 code 1 ")
    check(True, "the arrow of a SCROLLBAR control sends SB_LINEDOWN to the parent")
    vm.click_screen((vs[0] + vs[2]) // 2, vs[3] - 40, r"ctl 1 code 3 ")
    check(True, "the track below the thumb sends SB_PAGEDOWN")

    # the scroll bar of the window itself (WS_VSCROLL, in the margin right of the client area)
    vm.click_screen(cx + cw + 8, cy + ch - 8, r"window scroll code 1 ")
    check(True, "the margin scroll bar of a window sends WM_VSCROLL to the window")
    vm.mon("sendkey esc", 0.5)
    check(vm.wait_for("bye", timeout=10), "the program ends")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
