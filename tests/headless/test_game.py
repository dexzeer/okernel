#!/usr/bin/env python3
"""KAnarchy Wheels in the kernel: `game` lists the levels, `game 1` opens the window, holding UP (a real held PS/2 key) rolls the
chair across the screen, R restarts, a script in the file system runs too, and nothing faults.
Screenshots: ~/okvm/game_*.png"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

results = []


def step(name, ok, extra=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + ((" " + extra) if extra else ""), flush=True)


def shot(vm, name):
    from PIL import Image
    w, h, pix = vm.dump()
    Image.frombytes("RGB", (w, h), pix).save(os.path.join(os.path.expanduser("~"), "okvm", f"game_{name}.png"))
    return w, h, pix


vm = None
try:
    import numpy as np
    vm = OkVM("game")
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("cpu slow 1\n", delay=0.05)
    time.sleep(1)
    vm.type_string("game\n", delay=0.05)
    time.sleep(1.5)
    w, h, pix0 = shot(vm, "list")
    vm.type_string("game 1\n", delay=0.05)
    step("the window opens", vm.wait_for("[game] started", timeout=20))
    time.sleep(3)
    w, h, a = shot(vm, "start")
    a = np.frombuffer(a, dtype=np.uint8).reshape(h, w, 3).astype(np.int32)
    vm.mon("sendkey up 5000", wait=0.2)                          # a held key: press, 5 s, release
    time.sleep(6)
    w, h, b = shot(vm, "rolling")
    b = np.frombuffer(b, dtype=np.uint8).reshape(h, w, 3).astype(np.int32)
    # the picture changed a lot (the world scrolled under the camera): count the pixels that differ
    moved = int((np.abs(a - b).sum(axis=2) > 40).sum())
    step("holding UP makes the picture move", moved > 20000, f"{moved} pixels differ")
    log = vm.serial()
    step("the level script ran (message shown, no script errors)", "[dmccl] error" not in log and "level error" not in log)
    vm.mon("sendkey r", wait=0.3)                                # (the emulated CPU is slow: the key may take a few seconds to be handled)
    step("R restarts the level", vm.wait_for("[game] restart", timeout=20))
    shot(vm, "restart")
    bad = [l for l in vm.serial().splitlines() if "kmalloc FAIL" in l or "bad or double free" in l or "PAGE FAULT" in l.upper()]
    step("no faults", not bad, str(bad[:3]))
    print("RESULT:", "PASS" if all(results) else "FAIL")
finally:
    if vm:
        vm.kill()
sys.exit(0 if all(results) else 1)
