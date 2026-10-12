#!/usr/bin/env python3
"""Click tiles of the DuckDuckGo image challenge fixture (tests/web/captcha/ddg), screenshot, press Submit."""
import sys, os, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
out = sys.argv[1]
srv = subprocess.Popen([sys.executable, "-m", "http.server", "8002", "--directory", os.path.join(ROOT, "tests", "web", "captcha", "ddg")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
time.sleep(1)
vm = OkVM("capclick")
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("cpu slow 1\n", delay=0.05)
    time.sleep(1)
    vm.type_string("okai http://10.0.2.2:8002/html/anomaly.html\n", delay=0.05)
    vm.wait_for("sub-res done", timeout=60)
    time.sleep(3)
    from PIL import Image
    for (x, y) in ((863, 505), (1073, 510), (968, 710)):          # three duck tiles (screen px)
        vm.click_screen(x, y, r"\[mse\] btn=1")
        time.sleep(1.5)
    w, h, pix = vm.dump()
    Image.frombytes("RGB", (w, h), pix).save(out)
    vm.click_screen(968, 797, r"\[mse\] btn=1")                     # Submit
    time.sleep(8)
    print([l for l in vm.serial().splitlines() if "submit form" in l or "navigate" in l or "toggle" in l][-8:])
finally:
    vm.kill()
    srv.terminate()
