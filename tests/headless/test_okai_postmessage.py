#!/usr/bin/env python3
"""window.postMessage between frames (fixtures tests/web/captcha/frames/pm_parent.html + pm_child.html): the child
announces itself, the parent answers with structured data (Date, Map), a message with the wrong target origin is dropped,
e.source works for replying, parent/top/contentWindow identify the right frames."""
import sys, os, time, subprocess, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
srv = subprocess.Popen([sys.executable, "-m", "http.server", "8003", "--directory", os.path.join(ROOT, "tests", "web", "captcha", "frames")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
time.sleep(1)
results = []


def step(name, ok, extra=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + ((" " + extra) if extra else ""), flush=True)


vm = None
try:
    vm = OkVM("okaipm")
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("cpu slow 1\n", delay=0.05)
    time.sleep(1)
    vm.type_string("okai http://10.0.2.2:8003/pm_parent.html\n", delay=0.05)
    step("child announces itself to the parent", vm.wait_for("PMP got {\"hello\":1} origin=http://10.0.2.2:8003 fromFrame=true", timeout=90))
    step("parent -> child: structured data and the origin", vm.wait_for("PMC got {\"n\":1,\"d\":", timeout=30) and vm.wait_for("date=5 map=1,2", timeout=5))
    step("e.source.postMessage replies to the sender", vm.wait_for("PMC got {\"n\":2}", timeout=30))
    step("child -> parent after two messages", vm.wait_for("PMP got {\"done\":2}", timeout=30))
    log = vm.serial()
    step("wrong target origin is not delivered", "wrong-origin" not in log)
    step("identities", "PMP start top=true parent=true" in log and "PMC start framed=true sameTopParent=true" in log)
    bad = [l for l in log.splitlines() if "kmalloc FAIL" in l or "bad or double free" in l or "PAGE FAULT" in l.upper()]
    step("no faults", not bad, str(bad[:3]))
    print("RESULT:", "PASS" if all(results) else "FAIL")
finally:
    if vm:
        vm.kill()
    srv.terminate()
sys.exit(0 if all(results) else 1)
