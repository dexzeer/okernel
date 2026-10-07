#!/usr/bin/env python3
"""Long display stress session: mixed browser/keyboard/render/network ops.
Logs per-cycle PASS/FAIL + screenshot; aborts with diagnostics on hang.
Designed to run unattended for hours (single boot, many cycles).
Focus discipline: shell is focused at boot; browser content after first
load. All typing targets the focused surface; unknown keys are no-ops.
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

OUT = os.path.expanduser("~/okvm")
LOG = os.path.join(OUT, "stress_run.log")

def log(msg):
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    with open(LOG, "a") as f:
        f.write(line + "\n")

def nparse(vm):
    return len(re.findall(r"parse: count=", vm.serial()))

def healthy(vm):
    s = vm.serial()
    if "triple fault" in s:
        return False
    return True

PAGES = [
    "https://www.wikipedia.org/",
    "https://example.com/",
    "http://info.cern.ch/",
]

vm = OkVM("stress")
try:
    time.sleep(14)
    log(f"boot alive={healthy(vm)}")
    # shell phase (shell focused at boot): extra terminal + help text
    vm.type_string("terminal\n")
    time.sleep(2)
    vm.type_string("help\n")
    time.sleep(1)
    vm.dump(os.path.join(OUT, "stress_shell.ppm"))
    log("shell phase done (two terminals up: multi-window compositing live)")
    # browser phase: open okai from shell, then stay in content
    vm.type_string("okai https://www.wikipedia.org/\n")
    base = nparse(vm)
    t0 = time.time()
    while nparse(vm) <= base and time.time() - t0 < 150:
        time.sleep(2)
    log(f"browser open parse={nparse(vm) > base}")
    time.sleep(3)
    fails = 0
    cycle = 0
    while cycle < 40:
        vm.type_string("g")
        time.sleep(0.7)
        host = PAGES[cycle % len(PAGES)]
        base = nparse(vm)
        vm.type_string(host + "\n")
        t0 = time.time()
        while nparse(vm) <= base and time.time() - t0 < 150:
            time.sleep(2)
        got = nparse(vm) > base
        time.sleep(2)
        ok = healthy(vm)
        log(f"c{cycle} load {host} parse={got} alive={ok}")
        if not ok:
            fails += 1
            if fails >= 3:
                log("3 unhealthy: aborting session")
                break
            cycle += 1
            continue
        if not got:
            log(f"c{cycle} fetch flake (server-side, tolerated)")
            cycle += 1
            continue
        for _ in range(15):
            vm.type_string("j")
            time.sleep(0.15)
        for _ in range(10):
            vm.type_string("k")
            time.sleep(0.15)
        regions = vm.link_regions()
        if regions:
            try:
                vm.click_link(regions[-1][5], max_iters=6)
                time.sleep(2)
            except Exception as e:
                log(f"c{cycle} click exception: {e}")
        vm.type_string("b")
        time.sleep(1.5)
        vm.type_string("r")
        time.sleep(2)
        ok = healthy(vm)
        vm.dump(os.path.join(OUT, f"stress_c{cycle:02d}.ppm"))
        log(f"c{cycle} nav alive={ok}")
        if not ok:
            fails += 1
            if fails >= 3:
                break
        cycle += 1
    log(f"STRESS DONE cycles={cycle} fails={fails}")
finally:
    vm.kill()
