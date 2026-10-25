#!/usr/bin/env python3
"""Win32 GUI on user32.dll/gdi32.dll (ours) over /dev/okwin: painting, controls, timer, mouse and keyboard messages.

    sh tools/lx/build_tests.sh && python3 tests/headless/test_lx_win32gui.py
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

def find(px, w, h, color, x0=0, y0=0, x1=None, y1=None):
    x1 = x1 or w; y1 = y1 or h
    for y in range(y0, y1):
        for x in range(x0, x1):
            if px(x, y) == color: return (x, y)
    return None

vm = OkVM("w32gui")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "desktop up")
    time.sleep(8)
    vm.type_string("lx /win/guidemo.exe\n")
    check(vm.wait_for("window ready", timeout=60), "the program registered a class and created its window")
    time.sleep(1.5)
    shot = os.path.join(okvm.OUTDIR, "w32.ppm")
    vm.mon(f"screendump {shot}", 1.5)
    w, h, px = ppm(shot)
    red = find(px, w, h, (200, 0, 0))
    check(red is not None, "GDI Rectangle with a solid brush is on screen")
    if red:
        rx, ry = red                              # top-left of the red rectangle = client (20, 20)
        ox, oy = rx - 20, ry - 20                 # client origin on the screen
        check(px(ox + 150, oy + 50) == (0, 160, 0), "Ellipse (green)")
        check(px(ox + 260, oy + 70) == (0, 0, 200), "Polygon (blue triangle)")
        check(px(ox + 340, oy + 50) == (240, 200, 0), "BitBlt from a memory DC")
        check(px(ox + 300, oy + 100) == (0, 0, 0), "LineTo with the default pen")
        check(px(ox + 380, oy + 10) == (255, 255, 255), "window background from the class brush (COLOR_WINDOW)")
        face = px(ox + 70, oy + 174)
        check(face == (240, 240, 240), f"push button face is drawn ({face})")
        dark = sum(1 for yy in range(oy + 108, oy + 126) for xx in range(ox + 20, ox + 160) if sum(px(xx, yy)) < 300)
        check(dark > 60, f"text from TextOut/wsprintf ({dark} dark pixels)")
        # timer
        check(vm.wait_for("timer 3", timeout=15), "WM_TIMER arrives")
        # click the button
        vm.click_screen(ox + 70, oy + 174, r"clicked \d")
        check(vm.wait_for("clicked 1", timeout=15), "a click on the BUTTON control sends WM_COMMAND to the parent")
        # type into the edit control
        vm.premove(ox + 240, oy + 174, 4.8)
        vm.click()
        time.sleep(0.5)
        vm.type_string("hi")
        check(vm.wait_for("edit: hi", timeout=15), "typing into the EDIT control changes its text (EN_CHANGE, GetWindowText)")
        vm.premove(ox + 360, oy + 225, 4.8)      # empty client area: the window itself gets the keyboard
        vm.click()
        time.sleep(0.5)
        vm.mon("sendkey esc", 0.5)
        check(vm.wait_for("bye", timeout=15), "ESC -> DestroyWindow -> PostQuitMessage ends the message loop")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
