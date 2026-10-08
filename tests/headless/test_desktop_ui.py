#!/usr/bin/env python3
"""Desktop UI regressions (2026-10-08 report):

  1. no launcher button on the taskbar (KAnarchy is terminal-driven)
  2. hovering a taskbar button / okai caption button and moving away must
     not leave the hover highlight behind (a clipped cursor-erase repair used
     to re-render the cached chrome and show the new state only inside the
     cursor rect)
  3. clicking the terminal where it overlaps okai focuses the TERMINAL
     (clicks were routed by window slot index, not z-order: typing went to
     the browser underneath)

    python3 tests/headless/test_desktop_ui.py
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR
from PIL import Image

results = []
def check(name, ok, info=""):
    results.append(bool(ok))
    print(f"    {'PASS' if ok else 'FAIL'}  {name}  {info}", flush=True)

def shot(vm, name):
    w, h, px = vm.dump(os.path.join(OUTDIR, f"dui_{name}.ppm"))
    im = Image.frombytes("RGB", (w, h), px[:w * h * 3])
    im.save(os.path.join(OUTDIR, f"dui_{name}.png"))
    return im

def diff(a, b):
    return sum(abs(x - y) for x, y in zip(a, b))

def jiggle(vm, dx, dy, n=40):
    """Many tiny moves, like a real mouse: the stale-hover race needs the
    cursor to move between the chrome's own update and the cursor-erase
    repair of the same frame, which coarse bursts almost never hit."""
    for k in range(n):
        s = 1 if (k // 10) % 2 == 0 else -1
        vm.mon(f"mouse_move {dx * s} {dy * s}", 0.012)

vm = OkVM("dui")
try:
    vm.wait_for("[mem] heap", timeout=60); time.sleep(8)
    log = vm.serial()
    check("no launcher button", "[taskbar] layout n=" in log and "[taskbar] start" not in log)

    vm.type_string("okai\n")
    vm.wait_for("home rendered", timeout=40); time.sleep(3)

    # -- taskbar hover: the terminal's button (win 0, unfocused) -------------
    tb = vm.taskbar_rect(0)
    check("terminal taskbar button logged", tb, str(tb))
    if tb:
        bx, by, bw, bh = tb
        vm.anchor(1200, by + bh // 2)                  # empty taskbar: no-op click
        for _ in range(3):                             # in and out a few times
            vm.premove(bx + bw // 2, by + bh // 2); time.sleep(0.6)
            jiggle(vm, 0, -3)                          # across the top edge
            vm.burst(0, -30); time.sleep(0.6)          # up, onto the page
            vm.anchor(1200, by + bh // 2)
        time.sleep(1.5)
        im = shot(vm, "taskbar")
        y = by + 6                                     # above icon/text, below the pill
        inside = [im.getpixel((x, y)) for x in (bx + 3, bx + bw - 4)]
        outside = im.getpixel((bx + bw + 2, y))        # gap between buttons
        d = max(diff(p, outside) for p in inside)
        check("taskbar hover cleared", d < 24, f"inside={inside} gap={outside} d={d}")

    # -- okai caption hover: close (red when hovered) -------------------------
    cx, cy = vm.ui_center("close")
    cr = vm.ui_rect("close")
    vm.anchor(cx - 400, cy)                            # tab strip, empty
    for _ in range(3):
        vm.premove(cx, cy); time.sleep(0.6)
        jiggle(vm, 0, 3)                               # across the bottom edge
        vm.burst(0, 30); time.sleep(0.6)               # down, onto the page
        vm.anchor(cx - 400, cy)
    time.sleep(1.5)
    im = shot(vm, "caption")
    p = im.getpixel((cr[0] + 3, cr[1] + 3))
    check("okai close hover cleared", p[0] - p[1] < 60, f"corner={p}")

    # -- click focus follows z-order ------------------------------------------
    mx_, my_ = vm.ui_center("max")
    vm.click_ui("max", r"\[win\] \d+ restored", anchor=(mx_ - 400, my_))
    time.sleep(3)
    url = vm.ui_rect("url")
    tbt = vm.taskbar_rect(0)
    vm.anchor(1200, tbt[1] + tbt[3] // 2)
    vm.click_screen(tbt[0] + tbt[2] // 2, tbt[1] + tbt[3] // 2, r"\[mse\] btn=1")
    time.sleep(1.5)                                    # terminal raised + focused
    # a point inside the terminal body (16..884 x 168..748) that okai covers too
    ox0 = url[0] - 200                                 # okai's left edge is left of its URL bar
    px_, py_ = max(60, min(800, url[0] + 40)), 600
    overl = url[0] - 220 < px_ and url[1] < py_
    vm.click_screen(px_, py_, r"\[mse\] btn=1")
    time.sleep(1)
    vm.type_string("sysinfo\n"); time.sleep(3)
    got = vm.taskbar_rect(2) is not None or "[taskbar] btn win=2" in vm.serial()
    check("typing after clicking terminal over okai reaches the shell", got,
          f"click=({px_},{py_}) okai url={url} overlap~{overl}")
    shot(vm, "final")
finally:
    vm.kill()

faults = [l for l in vm.serial().splitlines()
          if "PAGE FAULT" in l.upper() or "[ISR] Exception" in l or "kmalloc FAIL" in l]
check("no faults", not faults, str(faults[:3]))
ok = all(results)
print("DESKTOP_UI", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
