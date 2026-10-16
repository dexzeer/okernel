#!/usr/bin/env python3
"""WebAssembly inside the kernel: tests/web/js/wasm/t_*.js run in okai (fixture server on :8005) and their console output
must equal the files node (V8) produced (t_*.expected) - the same comparison `tests/web/test_wasm.sh` does on the host.
Runs with the desktop page budgets (`cpu slow 1`), so the heavier tests are skipped here (t_random runs 20 programs)."""
import sys, os, time, re, subprocess, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "tests", "web", "js", "wasm")
WWW = os.path.join(OUTDIR, "www-wasm")
shutil.rmtree(WWW, ignore_errors=True)
os.makedirs(WWW)
for f in ("lib.js", "dis.js"):
    shutil.copy(os.path.join(SRC, f), os.path.join(WWW, f))
TESTS = ["t_api", "t_control", "t_memory", "t_numeric"]
for t in TESTS:
    shutil.copy(os.path.join(SRC, t + ".js"), os.path.join(WWW, t + ".js"))
    open(os.path.join(WWW, t + ".html"), "w").write('<!doctype html><html><body><script src="lib.js"></script><script src="%s.js"></script></body></html>' % t)
# a short random run: seeds 1..20 of the generated programs
shutil.copy(os.path.join(SRC, "t_random.js"), os.path.join(WWW, "t_random.js"))
open(os.path.join(WWW, "t_random.html"), "w").write('<!doctype html><html><body><script>var FIRST_SEED=1,LAST_SEED=20;</script><script src="lib.js"></script><script src="t_random.js"></script></body></html>')
TESTS.append("t_random")

srv = subprocess.Popen([sys.executable, "-m", "http.server", "8005", "--directory", WWW], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
time.sleep(1)
results = []


def step(name, ok, extra=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + ((" " + extra) if extra else ""), flush=True)


vm = None
try:
    vm = OkVM("okaiwasm")
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("cpu slow 1\n", delay=0.05)
    time.sleep(1)
    base = "http://10.0.2.2:8005/"
    done_seen = 0
    for i, t in enumerate(TESTS):
        if i == 0:
            vm.type_string("okai " + base + t + ".html\n", delay=0.05)
        else:
            time.sleep(1)
            vm.type_string("g")
            time.sleep(0.5)
            vm.type_string(base + t + ".html\n", delay=0.05)
        ok = False
        for _ in range(240):
            if vm.serial().count("[js] WASM-DONE") > done_seen:
                ok = True
                break
            time.sleep(1)
        if not ok:
            step(t, False, "no WASM-DONE")
            continue
        log = vm.serial()
        # the lines of this page: everything between the previous WASM-DONE and this one
        chunks = log.split("[js] WASM-DONE")
        body = chunks[done_seen]
        done_seen += 1
        mine = [l[5:] for l in body.splitlines() if l.startswith("[js] ") and not l.startswith("[js] load event:") and "further console output" not in l]
        want = open(os.path.join(SRC, t + ".expected"), encoding="utf-8").read().splitlines()
        want = [l for l in want if l != "WASM-DONE"]
        if t == "t_random":
            want = want[:20]
        bad = [(a, b) for a, b in zip(mine, want) if a != b]
        step(t, mine == want, "" if mine == want else "(%d lines vs %d, first mismatch %s)" % (len(mine), len(want), bad[:1]))
    bad = [l for l in vm.serial().splitlines() if "kmalloc FAIL" in l or "bad or double free" in l or "PAGE FAULT" in l.upper() or "abort()" in l]
    step("no faults / allocator errors", not bad, str(bad[:3]))
    print("RESULT:", "PASS" if all(results) else "FAIL")
finally:
    if vm:
        vm.kill()
    srv.terminate()
sys.exit(0 if all(results) else 1)
