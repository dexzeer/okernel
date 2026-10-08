#!/usr/bin/env python3
"""Keyboard integrity: type the same command many times (with and without
concurrent mouse motion) and check the shell received every line intact.
Catches dropped / reordered scancodes (i8042 byte-ownership races between
the keyboard and mouse IRQ handlers).

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
finally:
    vm.kill()
print("TYPING", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
