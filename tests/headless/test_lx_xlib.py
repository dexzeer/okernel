#!/usr/bin/env python3
"""A real Xlib program (host libX11, dynamically linked) against the X server xokai.

    HOSTBINS=1 sh tools/lx/build_tests.sh && python3 tests/headless/test_lx_xlib.py
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

vm = OkVM("xlib")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "desktop up")
    time.sleep(8)
    vm.type_string("lx /bin/xokai &\n")
    check(vm.wait_for("xokai: display :0 ready", timeout=40), "the server is up")
    time.sleep(1)
    vm.type_string("lx /bin/xl_xlib\n")
    check(vm.wait_for("xlib: font ascent", timeout=90), "XOpenDisplay, XCreateSimpleWindow, XLoadQueryFont work with real libX11")
    check(vm.wait_for("xlib: drawn pointer=1", timeout=40), "Expose arrives, the program draws, XQueryPointer answers")
    log = vm.serial()
    check("xlib: cannot open" not in log, "no connection failure")
    time.sleep(2.5)
    shot = os.path.join(okvm.OUTDIR, "xlib.ppm")
    vm.mon(f"screendump {shot}", 1.5)
    w, h, px = ppm(shot)
    red = None
    for y in range(40, 500):
        for x in range(40, 900):
            if px(x, y) == (192, 0, 0): red = (x, y); break
        if red: break
    check(red is not None, "the red rectangle is on the screen")
    if red:
        rx, ry = red
        check(px(rx + 110, ry) == (0, 0, 192), "the blue rectangle")
        check(px(rx + 220, ry + 20) == (0, 128, 0), "XFillArc: the green disc")
        dark = sum(1 for yy in range(ry + 98, ry + 116) for xx in range(rx, rx + 160) if sum(px(xx, yy)) < 200)
        check(dark > 80, f"XDrawString ({dark} dark pixels)")
        check(vm.click_screen(rx + 40, ry + 30, r"xlib: button 1 at"), "a click arrives as ButtonPress")
    vm.type_string("a")
    check(vm.wait_for("xlib: key keycode=38 keysym=61 str='a'", timeout=15), "a key press: keycode, keysym and XLookupString")
    vm.mon("sendkey esc", 0.5)
    check(vm.wait_for("xlib: bye", timeout=20), "ESC (keysym Escape) ends the program")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
