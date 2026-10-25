#!/usr/bin/env python3
"""Window chrome: rounded corners, app icon + name from the left of the title bar, open / minimize / restore animations.

    python3 tests/headless/test_winfx.py

Assumes the default terminal window (16,128, 868x620) of the desktop build.
"""
import sys, os, re, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
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

WX, WY, WW, WH, R = 16, 128, 868, 620, 12
BORDER = (70, 70, 78)          # HDR_BORDER (focused)

def near(a, b, t=6): return all(abs(x - y) <= t for x, y in zip(a, b))

vm = OkVM("winfx")
try:
    check(vm.wait_for("[mem] heap", timeout=60), "desktop boots")
    time.sleep(8)
    check(vm.wait_for(r"[winfx] open win=0", timeout=10), "the first window opened with an animation")
    n = int(re.findall(r"\[winfx\] open win=0 frames=(\d+)", vm.serial())[0])
    check(n >= 2, f"the opening animation drew frames ({n})")

    shot = os.path.join(okvm.OUTDIR, "winfx.ppm")
    vm.mon(f"screendump {shot}", 1.0)
    w, h, px = ppm(shot)
    check(near(px(WX + 60, WY), BORDER) and near(px(WX, WY + 60), BORDER), "window frame where expected")
    # rounded corners: the outer corner pixel is not window chrome any more, the frame runs along the arc
    for name, (cx, cy) in {"top-left": (WX, WY), "top-right": (WX + WW - 1, WY),
                           "bottom-left": (WX, WY + WH - 1), "bottom-right": (WX + WW - 1, WY + WH - 1)}.items():
        check(not near(px(cx, cy), BORDER, 14), f"{name} corner pixel is not frame colour {px(cx, cy)}")
    d = 4   # diagonal inside the arc: the ring must be there
    check(near(px(WX + 3, WY + 3), BORDER, 20) or near(px(WX + 4, WY + 4), BORDER, 20), "frame follows the arc")
    # icon + name from the left
    green = sum(1 for yy in range(WY + 4, WY + 40) for xx in range(WX + 12, WX + 40) if px(xx, yy)[1] > 150 and px(xx, yy)[0] < 140)
    check(green >= 4, f"terminal icon in the title bar ({green} green pixels)")
    def bright(x0, x1): return sum(1 for yy in range(WY + 8, WY + 36) for xx in range(x0, x1) if sum(px(xx, yy)) > 560)
    left, centre = bright(WX + 50, WX + 160), bright(WX + 340, WX + 520)
    check(left > 40 and centre == 0, f"title text starts at the left (left={left} centre={centre})")

    # minimize: the close/minimize buttons are at fixed offsets of the window
    mx, my = WX + WW - 2 - 8 - 30 - 36 + 15, WY + 2 + 5 + 15
    vm.click_screen(mx, my, r"\[win\] 0 minimized")
    check(vm.wait_for(r"[winfx] minimize win=0 frames=", timeout=10), "minimizing ran an animation")
    n = int(re.findall(r"\[winfx\] minimize win=0 frames=(\d+)", vm.serial())[0])
    check(n >= 2, f"the minimize animation drew frames ({n})")
    time.sleep(0.5)
    vm.mon(f"screendump {shot}", 1.0)
    w, h, px = ppm(shot)
    check(px(WX + 400, WY + 300) != (0, 0, 0), "the window is gone after minimizing")

    # restore from the taskbar
    vm.click_screen(100, 1056, r"restored from taskbar")
    check(vm.wait_for(r"[winfx] restore win=0 frames=", timeout=10), "restoring ran an animation")
    time.sleep(0.5)
    vm.mon(f"screendump {shot}", 1.0)
    w, h, px = ppm(shot)
    check(px(WX + 400, WY + 300) == (0, 0, 0), "the window is back after restoring")
    check(not near(px(WX, WY), BORDER, 14), "corners are still rounded after restoring")

    # close: the window fades out (the snapshot is taken while it exists, the animation runs after it is gone)
    cx = WX + WW - 2 - 8 - 30 + 15
    vm.click_screen(cx, my, r"\[winfx\] close win=0")
    check(vm.wait_for(r"[winfx] close win=0 frames=", timeout=10), "closing ran an animation")
    n = int(re.findall(r"\[winfx\] close win=0 frames=(\d+)", vm.serial())[0])
    check(n >= 2, f"the close animation drew frames ({n})")
    time.sleep(0.5)
    vm.mon(f"screendump {shot}", 1.0)
    w, h, px = ppm(shot)
    check(px(WX + 400, WY + 300) != (0, 0, 0), "the window is gone after closing")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
