#!/usr/bin/env python3
"""iframes in okai (fixture tests/web/captcha/frames): both frames render, a click on a link inside a frame navigates
THAT frame (not the page), the parent URL stays, the frame's document shows the new query."""
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
    vm = OkVM("okaiframes")
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("cpu slow 1\n", delay=0.05)
    time.sleep(1)
    vm.type_string("okai http://10.0.2.2:8003/parent.html\n", delay=0.05)
    step("a frame renders", vm.wait_for(" rendered ", timeout=60))
    time.sleep(8)
    step("two frames rendered", len(re.findall(r"\[okai\] frame \d+ rendered", vm.serial())) >= 2)
    n0 = vm.serial().count("[okai] navigate")
    vm.click_screen(136, 552, r"\[mse\] btn=1")             # the link in the second (260 px) frame
    time.sleep(8)
    log = vm.serial()
    step("the click navigates the frame, not the page", "[okai] navigate (frame) http://10.0.2.2:8003/child.html?n=3" in log and log.count("[okai] navigate") == n0 + 1)
    from PIL import Image
    w, h, pix = vm.dump()
    Image.frombytes("RGB", (w, h), pix).save(os.path.join(os.path.expanduser("~"), "okvm", "okaiframes_after.png"))
    bad = [l for l in log.splitlines() if "kmalloc FAIL" in l or "bad or double free" in l or "PAGE FAULT" in l.upper()]
    step("no faults", not bad, str(bad[:3]))
    print("RESULT:", "PASS" if all(results) else "FAIL")
finally:
    if vm:
        vm.kill()
    srv.terminate()
sys.exit(0 if all(results) else 1)
