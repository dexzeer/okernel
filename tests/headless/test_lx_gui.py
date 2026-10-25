#!/usr/bin/env python3
"""A Linux program draws into /dev/fb0 (a desktop window) and reads keys from /dev/input/event0.

    sh tools/lx/build_tests.sh && python3 tests/headless/test_lx_gui.py
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

vm = OkVM("lxgui")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "desktop up")
    time.sleep(8)
    vm.type_string("lx /bin/fbdemo\n")
    check(vm.wait_for("drawn", timeout=40), "program drew into /dev/fb0")
    log = vm.serial()
    check("fb 800x600 bpp 32" in log, "FBIOGET_VSCREENINFO reports 800x600x32")
    m = re.search(r"framebuffer window \d+x\d+ for pid \d+ content at (\d+),(\d+)", log)
    check(m is not None, "the framebuffer has a window")
    time.sleep(2)
    shot = os.path.join(okvm.OUTDIR, "lxgui.ppm")
    vm.mon(f"screendump {shot}", 1.5)
    ox, oy = int(m.group(1)), int(m.group(2))
    w, h, px = ppm(shot)
    check(px(ox + 100, oy + 100) == (255, 0, 0), f"red square on the screen {px(ox + 100, oy + 100)}")
    check(px(ox + 250, oy + 100) == (0, 0, 255), f"blue square on the screen {px(ox + 250, oy + 100)}")
    check(px(ox + 400, oy + 100) == (0, 255, 0), f"green square on the screen {px(ox + 400, oy + 100)}")
    vm.type_string("a")
    check(vm.wait_for("key 30", timeout=15), "key press arrives as an input event")
    check(vm.wait_for("\na\n", timeout=5), "the program reacted to it")
    vm.mon("sendkey esc", 0.5)
    check(vm.wait_for("bye", timeout=15), "ESC ends the program")
    check(vm.wait_for("exited, status 0", timeout=10), "clean exit")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
