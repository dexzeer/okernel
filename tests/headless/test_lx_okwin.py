#!/usr/bin/env python3
"""/dev/okwin: windows with shared pixel buffers, system-font text and input events.

    sh tools/lx/build_tests.sh && python3 tests/headless/test_lx_okwin.py
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

vm = OkVM("okw")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "desktop up")
    time.sleep(8)
    vm.type_string("lx /bin/okwtest\n")
    check(vm.wait_for("window ready", timeout=40), "the program created a window and drew into it")
    log = vm.serial()
    check("text width" in log, "OKW_TEXT measured text with the system font")
    time.sleep(2)
    shot = os.path.join(okvm.OUTDIR, "okw.ppm")
    vm.mon(f"screendump {shot}", 1.5)
    w, h, px = ppm(shot)
    ox, oy = 100 + 2, 80 + 36 + 2              # window x, y + border; y + title bar + border (WIN_TITLE_H = CHAR_H + 8)
    # find the client origin from the red square instead of trusting the title bar height
    red = None
    for y in range(60, 400):
        for x in range(60, 700):
            if px(x, y) == (192, 0, 0): red = (x, y); break
        if red: break
    check(red is not None, "red square is on the screen")
    if red:
        rx, ry = red
        check(px(rx + 120, ry) == (0, 160, 0) and px(rx + 240, ry) == (0, 0, 192), "green and blue squares next to it")
        dark = sum(1 for yy in range(ry + 100, ry + 140) for xx in range(rx, rx + 300) if sum(px(xx, yy)) < 300)
        check(dark > 150, f"anti-aliased text is drawn ({dark} dark pixels)")
        # a click inside the window
        vm.mouse_move_to(rx + 50, ry + 150) if hasattr(vm, "mouse_move_to") else None
    vm.type_string("a")
    check(vm.wait_for("char 97", timeout=15), "a typed character arrives as an event")
    check(vm.wait_for("key 30", timeout=5), "its key press arrives with a Linux key code")
    vm.mon("sendkey esc", 0.5)
    check(vm.wait_for("bye", timeout=15), "ESC ends the program")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
