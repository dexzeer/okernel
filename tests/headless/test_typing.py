#!/usr/bin/env python3
"""Keyboard integrity: type the same command many times (with and without
concurrent mouse motion) and check the shell received every line intact.
Catches dropped / reordered scancodes (i8042 byte-ownership races between
the keyboard and mouse IRQ handlers). Keys are queued by the IRQ and
dispatched by the main loop, so the Ctrl+Alt+T chord is checked too: Alt
must be sampled at press time, not when the loop gets to the key.

    python3 tests/headless/test_typing.py [rounds]
"""
import sys, os, time, re, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 8
WORD = "abcdefghijklmnopqrstuvwxyz"   # one token: [sh] exec logs the first word only

vm = OkVM("typing")
ok = True
try:
    vm.wait_for("[mem] heap", timeout=60); time.sleep(8)
    for phase in ("still", "mouse"):
        stop = [False]
        def wiggle():
            d = 1
            while not stop[0]:
                vm.mon(f"mouse_move {9 * d} {5 * d}", 0.02)
                d = -d
        th = None
        if phase == "mouse":
            th = threading.Thread(target=wiggle); th.start()
        mark = len(vm.serial())
        for _ in range(rounds):
            vm.type_string(WORD + "\n", delay=0.03)
            time.sleep(0.3)
        if th:
            stop[0] = True; th.join()
        time.sleep(2)
        lines = re.findall(r"\[sh\] exec: (.*)", vm.serial()[mark:])
        good = sum(1 for l in lines if l.strip() == WORD)
        bad = [l for l in lines if l.strip() != WORD]
        print(f"    {phase}: {good}/{rounds} intact  bad={bad[:4]}", flush=True)
        ok = ok and good == rounds
    # Global hotkey: a new terminal window appears (taskbar gains a button).
    n0 = [int(x) for x in re.findall(r"\[taskbar\] layout n=(\d+)", vm.serial())]
    vm.mon("sendkey ctrl-alt-t", 0.3)
    time.sleep(2)
    n1 = [int(x) for x in re.findall(r"\[taskbar\] layout n=(\d+)", vm.serial())]
    hot = bool(n1) and (not n0 or n1[-1] > n0[-1])
    print(f"    ctrl-alt-t: {'PASS' if hot else 'FAIL'}  layout {n0[-1:]} -> {n1[-1:]}", flush=True)
    ok = ok and hot
    # ...and the new (focused) terminal takes typing.
    mark = len(vm.serial())
    vm.type_string(WORD + "\n", delay=0.03)
    time.sleep(2)
    lines = re.findall(r"\[sh\] exec: (.*)", vm.serial()[mark:])
    fresh = any(l.strip() == WORD for l in lines)
    print(f"    new terminal typing: {'PASS' if fresh else 'FAIL'}  {lines[:2]}", flush=True)
    ok = ok and fresh
finally:
    vm.kill()
print("TYPING", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
