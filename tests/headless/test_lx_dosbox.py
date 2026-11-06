#!/usr/bin/env python3
"""A real emulator on the Linux subsystem: Ubuntu's DOSBox, on SDL 1.2 over SDL2 over Xlib, against xokai. Judged by the
screen, by files DOS programs write, and by the 1 Hz serial lines. The program comes from tools/lx/dosbox_root.sh; without
a copy there the test is skipped. Needs more than the default 512 MB of guest memory. A real game at real speed: tests/headless/doom_win.ps1.
"""
import os, re, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("OKVM_ISO", os.path.join(HERE, "..", "..", "build-host", "lxtest.iso"))
os.environ.setdefault("OKVM_MEM", "1024")
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

def exits(vm): return vm.serial().count("exited, status")

def wait_exit(vm, n, secs):
    end = time.time() + secs
    while time.time() < end and exits(vm) <= n: time.sleep(0.3)
    return exits(vm) > n

def shot(vm, name):
    p = os.path.join(okvm.OUTDIR, name + ".ppm")
    vm.mon(f"screendump {p}", 1.5)
    return ppm(p)

# Pixels inside the DOSBox window (the pointer starts in the middle of the screen, the window is placed around it).
def window_box(px, w, h):
    return [(x, y) for y in range(400, 760, 6) for x in range(660, 1260, 6)]

def run_after(vm, cmd, secs=20):
    n = exits(vm); s = len(vm.serial())
    vm.type_string(cmd + "\n")
    wait_exit(vm, n, secs)
    return vm.serial()[s:]

vm = OkVM("dosbox")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "desktop up")
    time.sleep(8)
    vm.type_string("lx /bin/xokai &\n")
    check(vm.wait_for("xokai: display :0 ready", timeout=40), "the server is up")
    time.sleep(1)

    # the prompt, typing, a file
    start = len(vm.serial()); n = exits(vm)
    vm.type_string("lx /bin/sh /bin/dosrun -c \"mount c /tmp\" -c c:\n")
    check(vm.wait_for("MIDI:Opened device", timeout=60), "DOSBox started")
    time.sleep(8)
    check(exits(vm) == n, "it is still running")
    w, h, px = shot(vm, "dosbox")
    blue = sum(1 for y in range(0, h, 4) for x in range(0, w, 4) if px(x, y) == (0, 0, 170))
    check(blue > 500, f"the blue welcome banner is on the screen ({blue})")
    vm.type_string("echo typed in dos > out.txt\n"); time.sleep(5)
    vm.type_string("exit\n")
    check(wait_exit(vm, n, 30), "DOSBox exits")
    seg = vm.serial()[start:]
    check(" exited, status 0" in seg, "with status 0")
    check("X Error" not in seg, "the X server took everything it sent")
    check("framebuffer window" not in seg, "looking for joysticks opens no window")
    seg = run_after(vm, "lx /bin/sh /bin/dosout")
    check("typed in dos" in seg, "keys reached DOS and DOS wrote a file" if "typed in dos" in seg else f"keys reached DOS and DOS wrote a file ({seg.strip()[-80:]!r})")

    # VGA graphics: what the program draws, how fast it gets to the desktop
    start = len(vm.serial()); n = exits(vm)
    vm.type_string("lx /bin/sh /bin/dosrun -c \"mount c /dos\" -c c: -c vgabench -c exit\n")
    time.sleep(12)
    w, h, px = shot(vm, "dosbox_a")
    a = [px(x, y) for x, y in window_box(px, w, h)]
    time.sleep(0.5)
    w, h, px = shot(vm, "dosbox_b")
    b = [px(x, y) for x, y in window_box(px, w, h)]
    check(len(set(a)) > 16, f"mode 13h is on the screen ({len(set(a))} colours)")
    check(a != b, "and it moves")
    check(wait_exit(vm, n, 240), "the benchmark ends")
    seg = vm.serial()[start:]
    pres = [int(v) for v in re.findall(r"\[okwin\] presents=(\d+)", seg)]
    print("     image updates a second:", " ".join(map(str, pres))[:300])
    check(pres and max(pres) >= 20, "the frames reach the window more than a few times a second")
    res = run_after(vm, "lx /bin/sh /bin/dosbench")
    m = re.search(r"frames (\d+) ticks (\d+)", res)
    check(m is not None and int(m.group(2)) >= 182, f"BENCH.TXT: {m.group(0) if m else res.strip()[-80:]!r}")
    if m: print(f"     {int(m.group(1)) * 18.2 / int(m.group(2)):.1f} frames per emulated second")

    # the mouse driver and the keyboard, from inside DOS
    start = len(vm.serial()); n = exits(vm)
    vm.type_string("lx /bin/sh /bin/dosrun -c \"mount c /dos\" -c c: -c inptest -c exit\n")
    time.sleep(12)
    vm.mon("mouse_button 1", 0.3); vm.mon("mouse_button 0", 1.5)                    # the first click takes the mouse, the way DOSBox does
    for _ in range(10): vm.mon("mouse_move 20 12", 0.25)
    time.sleep(1)
    vm.mon("mouse_button 1", 0.4); vm.mon("mouse_button 0", 0.8)
    pos = re.findall(r"\[mse\] btn=1 x=(\d+) y=(\d+)", vm.serial())
    if pos:
        check(640 <= int(pos[-1][0]) < 1282 and 382 <= int(pos[-1][1]) < 782, f"the captured pointer stays in the window ({pos[-1][0]},{pos[-1][1]})")
    vm.type_string("hi"); time.sleep(0.8)
    vm.mon("sendkey up", 0.8)
    shotw, shoth, spx = shot(vm, "dosbox_mouse")
    vm.mon("sendkey esc", 1)
    check(wait_exit(vm, n, 40), "the input program ends")
    res = run_after(vm, "lx /bin/sh /bin/dosinput")
    m = re.search(r"mouse (\d+) mx (-?\d+) my (-?\d+) btn (\d+) scan (\d+) text (\S*)", res)
    check(m is not None, f"INPUT.TXT: {m.group(0) if m else res.strip()[-120:]!r}")
    if m:
        check(m.group(1) == "65535", "the mouse driver is there")
        check(int(m.group(2)) > 20 and int(m.group(3)) > 20, "moving right and down reaches DOS as motion, past where the window would end")
        check(int(m.group(4)) & 1 == 1, "the left button reaches DOS")
        check(m.group(5) == "72", "an arrow key reaches DOS as its scan code")
        check(m.group(6) == "hi", "typed text reaches DOS")

    check(not re.search(r"\[(ISR|panic)\]", vm.serial()), "no kernel faults")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails)[:600])
sys.exit(1 if fails else 0)
