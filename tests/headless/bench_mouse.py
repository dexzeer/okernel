#!/usr/bin/env python3
"""Mouse-motion cost benchmark. Wiggles the cursor (as fast as the monitor
accepts moves) over different scene parts and reports, per scenario, the
presented FPS and main-loop iterations per second from the kernel's 1 Hz
`[fps] N loops=M` lines. FPS is input-bound here (it counts frames that
showed something new); `loops` is the headroom: every iteration polls the
network, input and okai, so per-frame mouse cost shows up as fewer loops.

    python3 tests/headless/bench_mouse.py [tag]
    OKVM_ISO=path  -> another build (A/B)
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

tag = sys.argv[1] if len(sys.argv) > 1 else "bmouse"

def stats(vm, mark):
    rows = re.findall(r"\[fps\] (\d+) loops=(\d+)", vm.serial()[mark:])
    rows = [(int(a), int(b)) for a, b in rows][1:]   # first second is partial
    if not rows:
        return "n/a"
    f = sorted(r[0] for r in rows); l = sorted(r[1] for r in rows)
    return f"fps_med={f[len(f) // 2]} loops_med={l[len(l) // 2]} n={len(rows)}"

def wiggle(vm, secs, dx=6, dy=3):
    end = time.time() + secs
    d = 1
    while time.time() < end:
        for _ in range(4):
            vm.mon(f"mouse_move {dx * d} {dy * d}", 0.008)
        d = -d

vm = OkVM(tag)
try:
    vm.wait_for("[mem] heap", timeout=60); time.sleep(8)
    mark = len(vm.serial()); time.sleep(6)
    print("BENCH idle", stats(vm, mark), flush=True)

    mark = len(vm.serial()); wiggle(vm, 8); time.sleep(1.1)
    print("BENCH wallpaper", stats(vm, mark), flush=True)

    vm.premove(450, 150); time.sleep(1)              # terminal title bar
    mark = len(vm.serial()); wiggle(vm, 8, 4, 1); time.sleep(1.1)
    print("BENCH titlebar", stats(vm, mark), flush=True)

    vm.type_string("okai\n")
    vm.wait_for("home rendered", timeout=40); time.sleep(3)
    mark = len(vm.serial()); wiggle(vm, 8); time.sleep(1.1)
    print("BENCH okai", stats(vm, mark), flush=True)
finally:
    vm.kill()
