#!/usr/bin/env python3
"""Win32 menus on user32: menu bar, popups, submenu, mnemonics, accelerator, context menu (TrackPopupMenu).

    sh tools/lx/build_tests.sh && python3 tests/headless/test_lx_menus.py
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

def ppm(path):
    d = open(path, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", d)
    w, h = int(m.group(1)), int(m.group(2))
    body = d[m.end():]
    return w, h, (lambda x, y: tuple(body[(y * w + x) * 3:(y * w + x) * 3 + 3]))

vm = OkVM("menus")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "desktop up")
    time.sleep(8)
    vm.type_string("lx /win/menudemo.exe\n")
    check(vm.wait_for("menu ready", timeout=60), "window with a menu bar created")
    log = vm.serial()
    bars = re.findall(r"bar(\d) (\d+),(\d+)(?:\r?\nEV [^\n]*)* (\d+),(\d+)", log)
    check(len(bars) == 2, f"GetMenuItemRect gives the bar items {bars}")
    b0 = tuple(int(v) for v in bars[0][1:]); b1 = tuple(int(v) for v in bars[1][1:])
    time.sleep(1.5)
    shot = os.path.join(okvm.OUTDIR, "menus.ppm")

    def snap():
        vm.mon(f"screendump {shot}", 1.2)
        return ppm(shot)

    w, h, px = snap()
    check(px(b0[0] + 3, b0[1] + 3) == (240, 240, 240), f"menu bar is painted {px(b0[0] + 3, b0[1] + 3)}")

    # open File with the mouse, take the second item (Open) with the keyboard
    cx, cy = (b0[0] + b0[2]) // 2, (b0[1] + b0[3]) // 2
    vm.click_screen(cx, cy, r"initpopup"); time.sleep(1.0)
    w, h, px = snap()
    pop = px(b0[0] + 20, b0[3] + 20)
    check(pop == (248, 248, 248) or pop == (205, 225, 245), f"File popup is open below the bar item {pop}")
    vm.mon("sendkey down", 0.3); vm.mon("sendkey ret", 0.5)
    check(vm.wait_for(r"cmd 101 src 0", timeout=10), "Down + Enter chooses the first item (WM_COMMAND 101 from the menu)")

    # mnemonic: File, then x = Exit is later; first test Edit > More > Deep with the keyboard
    vm.click_screen((b1[0] + b1[2]) // 2, (b1[1] + b1[3]) // 2, r"initpopup"); time.sleep(1.0)
    vm.type_string("m"); time.sleep(0.8)                                   # &More opens the submenu
    w, h, px = snap()
    vm.type_string("d"); time.sleep(0.5)                                   # &Deep item
    check(vm.wait_for(r"cmd 301 src 0", timeout=10), "mnemonics open a submenu and choose an item (Edit > More > Deep)")

    # accelerator Ctrl+N
    vm.mon("sendkey ctrl-n", 0.5)
    check(vm.wait_for(r"cmd 101 src 1", timeout=10), "Ctrl+N is translated by the accelerator table (src 1)")

    # context menu: right click in the client area
    vm.premove(b0[0] + 150, b0[3] + 120, 4.8); time.sleep(0.4)
    vm.mon("mouse_button 2", 0.2); vm.mon("mouse_button 0", 0.5)
    time.sleep(1.0)
    w, h, px = snap()
    pop = px(b0[0] + 150 + 30, b0[3] + 120 + 12)
    check(pop in ((248, 248, 248), (205, 225, 245)), f"the context menu opens at the pointer {pop}")
    vm.mon("sendkey down", 0.3); vm.mon("sendkey ret", 0.5)
    check(vm.wait_for(r"context result 401", timeout=10), "TPM_RETURNCMD returns the chosen command")

    # File > Exit by mnemonics ends the program
    vm.click_screen(cx, cy, r"initpopup"); time.sleep(1.0)
    vm.type_string("x")
    check(vm.wait_for(r"cmd 103 src 0", timeout=10), "File > Exit by its mnemonic")
    check(vm.wait_for("bye", timeout=10), "the program ends")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
