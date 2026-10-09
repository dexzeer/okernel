#!/usr/bin/env python3
"""Damage-tracking self check: after a mix of drawing (typing, new windows, a
browser page, wheel scrolling, window dragging, minimize/restore) the kernel's
`gfxcheck` command flushes and compares the framebuffer with the backbuffer
(outside the cursor sprite). Any pixel that was drawn but never marked dirty
(or flushed from the wrong span) shows up as a mismatch.

    python3 tests/headless/test_gfx_verify.py
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

vm = OkVM("gfxverify")
ok = True

def check(label):
    global ok
    n0 = len(re.findall(r"\[gfx\] verify:", vm.serial()))
    vm.type_string("gfxcheck\n", delay=0.03)
    t0 = time.time()
    while time.time() - t0 < 20:
        m = re.findall(r"\[gfx\] verify: (\d+) mismatching pixels \(first at (-?\d+),(-?\d+)\)", vm.serial())
        if len(m) > n0:
            bad, x, y = m[-1]
            good = int(bad) == 0
            print(f"    {'PASS' if good else 'FAIL'}  {label}: {bad} mismatches (first at {x},{y})", flush=True)
            ok = ok and good
            return
        time.sleep(0.5)
    print(f"    FAIL  {label}: gfxcheck did not answer", flush=True)
    ok = False

try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    check("fresh desktop")
    vm.type_string("help --a\n", delay=0.03); time.sleep(1)
    check("terminal text")
    vm.mon("sendkey ctrl-alt-t", 0.3); time.sleep(2)
    vm.type_string("echo second terminal window\n", delay=0.03); time.sleep(1)
    check("second terminal")

    vm.type_string("okai\n", delay=0.03)
    vm.wait_for("home rendered", timeout=40)
    time.sleep(2)
    vm.mon("sendkey ctrl-alt-t", 0.3); time.sleep(2)     # a terminal above okai to type into
    check("okai home page")

    # wheel over the page (hmp: dz > 0 scrolls up on the real path; both directions)
    vm.premove(700, 500)
    for dz in (1, 1, 1, -1, -1, -1, -1, -1, 1):
        vm.mon(f"mouse_move 0 0 {dz}", 0.25)
    time.sleep(1)
    check("after wheel scroll")

    # drag the terminal by its title bar
    vm.premove(500, 150)
    vm.mon("mouse_button 1", 0.3)
    for _ in range(12):
        vm.mon("mouse_move 14 9", 0.08)
    vm.mon("mouse_button 0", 0.3)
    time.sleep(1)
    vm.mon("sendkey ctrl-alt-t", 0.3); time.sleep(2)       # the drag may have focused the page: fresh terminal
    check("after window drag")

    # many small keystroke updates
    vm.type_string("echo " + "x" * 40 + "\n", delay=0.02)
    for i in range(6):
        vm.type_string(f"echo line {i}\n", delay=0.02)
    time.sleep(1)
    check("after typing")

    # random wiggle: cursor motion must not leave residue in the scene
    for _ in range(10):
        vm.mon("mouse_move 40 25", 0.05)
        vm.mon("mouse_move -40 -25", 0.05)
    time.sleep(1)
    check("after mouse wiggle")

    log = vm.serial()
    bad = re.findall(r"\[ISR\][^\n]*", log)
    if bad:
        print("    FAIL  kernel exception:", bad[:2]); ok = False
finally:
    vm.kill()

print("GFX_VERIFY", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
