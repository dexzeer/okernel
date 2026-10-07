#!/usr/bin/env python3
"""UI responsiveness benchmark: boot time, page load, scroll repaint cost and
presented frames per second while the mouse moves / the page scrolls.

    python3 tests/headless/bench_ui.py [tag] [url]
    OKVM_ISO=path  -> benchmark another build (A/B)

Prints one BENCH line per metric (serial log = ground truth: [fps] is logged
once a second, [okai] render lines carry their ms).
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

tag = sys.argv[1] if len(sys.argv) > 1 else "bench"
url = sys.argv[2] if len(sys.argv) > 2 else "https://en.wikipedia.org/wiki/Operating_system"

def fps_since(vm, mark):
    lines = vm.serial()[mark:]
    return [int(v) for v in re.findall(r"\[fps\] (\d+)", lines)]

t0 = time.time()
vm = OkVM(tag)
try:
    vm.wait_for("[mem] heap", timeout=90)
    t_heap = time.time() - t0
    vm.wait_for("[fps]", timeout=60)
    time.sleep(6)
    print(f"BENCH boot_to_heap_s {t_heap:.1f}", flush=True)

    # mouse motion over the desktop: frames presented per second
    mark = len(vm.serial())
    end = time.time() + 8
    d = 1
    while time.time() < end:
        for _ in range(6):
            vm.mon(f"mouse_move {12 * d} {7 * d}", 0.03)
        d = -d
    time.sleep(1.2)
    f = fps_since(vm, mark)
    f = sorted(f)[1:-1] if len(f) > 3 else f
    print(f"BENCH desktop_motion_fps {sum(f) / max(1, len(f)):.1f} {f}", flush=True)

    t1 = time.time()
    vm.type_string(f"okai {url}\n")
    vm.wait_for("page origin", timeout=150)
    t_first = time.time() - t1
    time.sleep(20)
    print(f"BENCH first_render_s {t_first:.1f}", flush=True)

    # browser motion: hover across the chrome + page
    mark = len(vm.serial())
    end = time.time() + 8
    while time.time() < end:
        for _ in range(6):
            vm.mon(f"mouse_move {14 * d} {5 * d}", 0.03)
        d = -d
    time.sleep(1.2)
    f = fps_since(vm, mark)
    f = sorted(f)[1:-1] if len(f) > 3 else f
    print(f"BENCH browser_motion_fps {sum(f) / max(1, len(f)):.1f} {f}", flush=True)

    # scrolling: mouse wheel over the page (goes to the window under the
    # cursor — j/k need keyboard focus on the page and silently no-op when
    # the address bar has it), repaint cost from the scroll log
    mark = len(vm.serial())
    for _ in range(3):
        for _ in range(12):
            vm.mon("mouse_move 0 0 1", 0.08)
        for _ in range(12):
            vm.mon("mouse_move 0 0 -1", 0.08)
    time.sleep(3)
    seg = vm.serial()[mark:]
    rs = sorted(int(v) for v in re.findall(r"\[okai\] scroll dy=-?\d+ in (\d+)kcyc", seg))
    med = rs[len(rs) // 2] if rs else 0
    f = fps_since(vm, mark)
    print(f"BENCH scroll_fps {sum(f) / max(1, len(f)):.1f} scrolls={len(rs)} median_kcyc={med}", flush=True)
finally:
    vm.kill()
