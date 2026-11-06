#!/usr/bin/env python3
"""A real emulator on the Linux subsystem: Ubuntu's DOSBox, on SDL 1.2 over SDL2 over Xlib, against xokai. Judged by the
screen and by a file DOS writes. The program comes from tools/lx/dosbox_root.sh; without a copy there the test is skipped.
Needs 2 GB of guest memory.
"""
import os, re, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("OKVM_ISO", os.path.join(HERE, "..", "..", "build-host", "lxtest.iso"))
os.environ.setdefault("OKVM_MEM", "2048")
import okvm
from okvm import OkVM

ROOT = os.path.join(HERE, "..", "..", "build-host", "extra_real")
if not os.path.exists(os.path.join(ROOT, "bin", "dosbox")):
    print("RESULT SKIP: no dosbox in build-host/extra_real")
    sys.exit(0)

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

def wait_exit(vm, n, secs):
    end = time.time() + secs
    while time.time() < end and vm.serial().count("exited, status") <= n: time.sleep(0.3)
    return vm.serial().count("exited, status") > n

vm = OkVM("dosbox")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "desktop up")
    time.sleep(8)
    vm.type_string("lx /bin/xokai &\n")
    check(vm.wait_for("xokai: display :0 ready", timeout=40), "the server is up")
    time.sleep(1)
    start = len(vm.serial())
    n = vm.serial().count("exited, status")
    vm.type_string("lx /bin/sh /bin/dosrun -c \"mount c /tmp\" -c c:\n")
    check(vm.wait_for("MIDI:Opened device", timeout=60), "DOSBox started")
    time.sleep(8)
    check(vm.serial().count("exited, status") == n, "it is still running")

    shot = os.path.join(okvm.OUTDIR, "dosbox.ppm")
    vm.mon(f"screendump {shot}", 1.5)
    w, h, px = ppm(shot)
    blue = sum(1 for y in range(0, h, 4) for x in range(0, w, 4) if px(x, y) == (0, 0, 170))
    check(blue > 500, f"the blue welcome banner is on the screen ({blue})")

    vm.type_string("echo typed in dos > out.txt\n"); time.sleep(5)
    vm.type_string("exit\n")
    check(wait_exit(vm, n, 30), "DOSBox exits")
    seg = vm.serial()[start:]
    check(" exited, status 0" in seg, "with status 0")
    check("X Error" not in seg, "the X server took everything it sent")
    check("framebuffer window" not in seg, "looking for joysticks opens no window")

    n = vm.serial().count("exited, status")
    start = len(vm.serial())
    vm.type_string("lx /bin/sh /bin/dosout\n")
    wait_exit(vm, n, 20)
    seg = vm.serial()[start:]
    check("typed in dos" in seg, f"keys reached DOS and DOS wrote a file ({seg.strip()[-80:]!r})" if "typed in dos" not in seg else "keys reached DOS and DOS wrote a file")
    check(not re.search(r"\[(ISR|panic)\]", vm.serial()), "no kernel faults")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails)[:600])
sys.exit(1 if fails else 0)
