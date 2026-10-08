#!/usr/bin/env python3
"""Keys pressed while the kernel boots must not break the keyboard.

The PS/2 init used to read the controller config byte without waiting for
it: a pending scancode was read instead and written back as the config,
clearing the set-1 translate bit — afterwards 'a' typed Enter, 's' typed ']'
and every key release typed the key again (or the keyboard IRQ was off).
This spams keystrokes + mouse motion from power-on until the heap line, then
checks the controller kept translation + both IRQs, the mouse reset/wheel
handshake read its replies in order, and typing arrives intact.

    python3 tests/headless/test_boot_keys.py
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

WORD = "abcdefghijklmnopqrstuvwxyz"
results = []
def check(name, ok, info=""):
    results.append(bool(ok))
    print(f"    {'PASS' if ok else 'FAIL'}  {name}  {info}", flush=True)

vm = OkVM(sys.argv[1] if len(sys.argv) > 1 else "bootkeys")
try:
    t0 = time.time()
    while not os.path.exists(vm.SOCK) and time.time() - t0 < 20:
        time.sleep(0.05)
    while "[mem] heap" not in vm.serial() and time.time() - t0 < 90:
        vm.mon("sendkey a", 0.01)
        vm.mon("mouse_move 3 2", 0.01)
    time.sleep(8)
    s = vm.serial()
    m = re.search(r"\[ps2\] config was=0x([0-9a-f]+) now=0x([0-9a-f]+) reset=([0-9a-f-]+)/([0-9a-f-]+)/([0-9a-f-]+)", s)
    check("ps2 config line", m)
    if m:
        now = int(m.group(2), 16)
        check("translation + IRQ1 + IRQ12 kept", (now & 0x43) == 0x43, f"now=0x{now:02x}")
        check("mouse reset replies in order", m.group(3, 4, 5) == ("fa", "aa", "0"), str(m.group(3, 4, 5)))
    check("wheel mode negotiated", "4-byte mode ON" in s)
    mark = len(vm.serial())
    for _ in range(3):
        vm.type_string(WORD + "\n", delay=0.03)
        time.sleep(0.4)
    time.sleep(2)
    lines = re.findall(r"\[sh\] exec: (.*)", vm.serial()[mark:])
    good = sum(1 for l in lines if l.strip() == WORD)
    check("typing intact after boot-time keys", good == 3, f"{good}/3 {lines[:3]}")
finally:
    vm.kill()
ok = all(results)
print("BOOT_KEYS", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
