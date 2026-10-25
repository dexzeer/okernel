#!/usr/bin/env python3
"""The X11 server (xokai): a raw-protocol client creates windows, draws shapes/text/pixmaps and reads input events.

    sh tools/lx/build_tests.sh && python3 tests/headless/test_lx_x11.py
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

vm = OkVM("x11")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "desktop up")
    time.sleep(8)
    vm.type_string("lx /bin/xokai &\n")
    check(vm.wait_for("xokai: display :0 ready", timeout=40), "the server listens on /tmp/.X11-unix/X0")
    time.sleep(1)
    vm.type_string("lx /bin/xtest\n")
    check(vm.wait_for("xtest: connected", timeout=40), "a client connects and gets the setup reply")
    check(vm.wait_for("xtest: mapped", timeout=40), "window creation, properties, font, GC, pixmap, map and a round trip work")
    check(vm.wait_for("xtest: orange pixel 16753920", timeout=20), "AllocNamedColor answers with a TrueColor pixel")
    check(vm.wait_for("xtest: wm_name len 5", timeout=20), "GetProperty returns what ChangeProperty stored")
    check(vm.wait_for("xtest: expose 1", timeout=30), "the window gets an Expose event")
    check(vm.wait_for("xtest: drawn", timeout=30), "the client drew in response")
    time.sleep(2.5)
    shot = os.path.join(okvm.OUTDIR, "x11.ppm")
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
        check(px(rx + 120, ry) == (0, 160, 0) and px(rx + 240, ry) == (0, 0, 192), "green and blue rectangles next to it")
        check(px(rx + 10, ry + 10) == (192, 0, 0), "filled")
        # disc (orange) at window (30..100, 150..220); pixmap copy (cyan) at (300,150); child (yellow) at (300,220)
        ox, oy = rx - 20, ry - 20
        check(px(ox + 65, oy + 185) == (255, 128, 0), "filled arc: the orange disc")
        check(px(ox + 332, oy + 182) == (0, 192, 192), "CopyArea from a pixmap")
        child_yellow = sum(1 for yy in range(oy + 220, oy + 280) for xx in range(ox + 300, ox + 380) if px(xx, yy) == (255, 255, 0))
        check(child_yellow > 3000, f"the child window is yellow ({child_yellow} px)")
        dark = sum(1 for yy in range(oy + 238, oy + 256) for xx in range(ox + 20, ox + 190) if sum(px(xx, yy)) < 200)
        check(dark > 100, f"core font text is drawn ({dark} dark pixels)")
        line = sum(1 for yy in range(oy + 95, oy + 135) for xx in range(ox + 20, ox + 380) if px(xx, yy) == (0, 0, 0))
        check(line > 200, f"wide polyline ({line} px)")
        tri = sum(1 for yy in range(oy + 150, oy + 221) for xx in range(ox + 140, ox + 241) if px(xx, yy) == (128, 0, 192))
        check(tri > 1500, f"filled polygon ({tri} px)")
        # input: a click in the window, then ESC
        check(vm.click_screen(rx + 50, ry + 40, r"xtest: button 1 at"), "a mouse click arrives as ButtonPress")
    vm.type_string("a")
    check(vm.wait_for("xtest: key 38", timeout=15), "a key press arrives with the X keycode (Linux code + 8)")
    vm.mon("sendkey esc", 0.5)
    check(vm.wait_for("xtest: bye", timeout=15), "ESC ends the client")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
