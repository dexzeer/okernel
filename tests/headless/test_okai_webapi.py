#!/usr/bin/env python3
"""The web API pages (tests/web/js/api_*.html, blob.html) inside the kernel realm (offline, fixture server on :8004).

The same pages run on the host in `make web-tests`; this runs them under the kernel's allocator, x87 float setup and
page budgets, which is where the natives (crypto, deflate, media, frames) really live. Each page ends with
"[js] RESULT ALLPASS" on its console.
PASS requires every page and no faults / allocator errors / realm aborts.
"""
import sys, os, time, re, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PAGES = ["blob.html", "api_messaging.html", "api_streams.html", "api_subtle.html", "api_idb.html", "api_worker.html", "api_misc.html"]
srv = subprocess.Popen([sys.executable, "-m", "http.server", "8004", "--directory", os.path.join(ROOT, "tests", "web", "js")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
time.sleep(1)
results = []


def step(name, ok, extra=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + ((" " + extra) if extra else ""), flush=True)


vm = None
try:
    vm = OkVM("okaiwebapi")
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("cpu slow 1\n", delay=0.05)
    time.sleep(1)
    base = "http://10.0.2.2:8004/"
    for i, page in enumerate(PAGES):
        n0 = len(re.findall(r"\[js\] RESULT (ALLPASS|FAIL)", vm.serial()))
        if i == 0:
            vm.type_string("okai " + base + page + "\n", delay=0.05)
        else:
            time.sleep(1)
            vm.type_string("g")
            time.sleep(0.5)
            vm.type_string(base + page + "\n", delay=0.05)
        ok = False
        for _ in range(120):
            log = vm.serial()
            m = re.findall(r"\[js\] RESULT (ALLPASS|FAIL)[^\n]*", log)
            if len(m) > n0:
                ok = m[-1] == "ALLPASS"
                break
            time.sleep(1)
        step(page, ok)
        if not ok:
            lines = [l for l in vm.serial().splitlines() if l.startswith("FAIL ") or "RESULT FAIL" in l]
            print("   ", lines[-6:])
    log = vm.serial()
    bad = [l for l in log.splitlines() if "kmalloc FAIL" in l or "bad or double free" in l or "PAGE FAULT" in l.upper() or "abort()" in l]
    step("no faults / allocator errors", not bad, str(bad[:3]))
    print("RESULT:", "PASS" if all(results) else "FAIL")
finally:
    if vm:
        vm.kill()
    srv.terminate()
sys.exit(0 if all(results) else 1)
