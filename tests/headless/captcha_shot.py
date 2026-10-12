#!/usr/bin/env python3
"""Load a captcha fixture from tests/web/captcha/<dir> in okai and save a screenshot (PNG) -- a viewing helper.
usage: captcha_shot.py <fixture dir under tests/web/captcha> <path> <out.png> [wait seconds]"""
import sys, os, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
d, path, out = sys.argv[1], sys.argv[2], sys.argv[3]
wait = float(sys.argv[4]) if len(sys.argv) > 4 else 12
srv = subprocess.Popen([sys.executable, "-m", "http.server", "8002", "--directory", os.path.join(ROOT, "tests", "web", "captcha", d)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
time.sleep(1)
vm = OkVM("capshot")
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("cpu slow 1\n", delay=0.05)
    time.sleep(1)
    vm.type_string("okai http://10.0.2.2:8002/" + path + "\n", delay=0.05)
    time.sleep(wait)
    from PIL import Image
    w, h, pix = vm.dump()
    Image.frombytes("RGB", (w, h), pix).save(out)
    print("saved", out)
finally:
    vm.kill()
    srv.terminate()
