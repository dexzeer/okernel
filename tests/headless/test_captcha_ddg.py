#!/usr/bin/env python3
"""DuckDuckGo's image challenge ("select all squares containing a duck"), offline fixture tests/web/captcha/ddg:
  1. the page renders (title, 9 tiles, Submit)
  2. clicking a tile (an image inside a <label for=...> whose checkbox is display:none) checks its box: the screenshot
     gets the blue frame + check mark, [okai] logs the label click and the toggle
  3. Submit sends a real POST (urlencoded) with exactly the checked tiles; the server's 302 is followed with a GET
"""
import sys, os, time, re, threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIX = os.path.join(ROOT, "tests", "web", "captcha", "ddg")
posts = []
gets = []


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        gets.append(self.path)
        if path == "/html/after.html":
            data = b"<!doctype html><title>after</title><h1>Challenge passed</h1>"
            ct = "text/html"
        else:
            fp = os.path.join(FIX, path.lstrip("/"))
            if not os.path.isfile(fp):
                self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
            data = open(fp, "rb").read()
            if path.endswith("anomaly.html"):
                data = data.replace(b"//duckduckgo.com/anomaly.js", b"/anomaly.js")
            ct = "text/html" if path.endswith(".html") else "text/css" if path.endswith(".css") else "image/jpeg"
        self.send_response(200)
        self.send_header("Content-Type", ct)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        posts.append((self.path, dict(self.headers), self.rfile.read(n)))
        self.send_response(302)
        self.send_header("Location", "/html/after.html")
        self.send_header("Content-Length", "0")
        self.end_headers()


srv = HTTPServer(("0.0.0.0", 8004), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
results = []


def step(name, ok, extra=""):
    results.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name + ((" " + extra) if extra else ""), flush=True)


vm = None
try:
    import numpy as np
    vm = OkVM("captchaddg")
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("cpu slow 1\n", delay=0.05)        # QEMU/TCG benchmarks as a slow CPU; irrelevant here, kept for stable timing
    time.sleep(1)
    vm.type_string("okai http://10.0.2.2:8004/html/anomaly.html\n", delay=0.05)
    step("the challenge page loads", vm.wait_for("sub-res done", timeout=60))
    time.sleep(3)
    w, h, pix = vm.dump()
    before = np.frombuffer(pix, dtype=np.uint8).reshape(h, w, 3).astype(np.int32)
    tiles = [(863, 505), (1073, 510), (968, 710)]    # screen px of three tiles (1920x1080 desktop)
    for (x, y) in tiles:
        vm.click_screen(x, y, r"\[mse\] btn=1")
        time.sleep(1.5)
    log = vm.serial()
    step("label clicks reach the checkboxes", log.count("[okai] label click -> control") >= 3 and log.count("[okai] toggle node=") >= 3)
    w, h, pix = vm.dump()
    after = np.frombuffer(pix, dtype=np.uint8).reshape(h, w, 3).astype(np.int32)
    blue = lambda im: int(((abs(im[:, :, 0] - 0x39) < 25) & (abs(im[:, :, 1] - 0x69) < 25) & (abs(im[:, :, 2] - 0xEF) < 25))[440:780, 800:1140].sum())
    step("the selected tiles are framed in blue", blue(after) > blue(before) + 300, f"{blue(before)} -> {blue(after)}")
    vm.click_screen(968, 797, r"\[mse\] btn=1")        # Submit
    step("Submit sends a POST", vm.wait_for("submit form (POST", timeout=15))
    time.sleep(6)
    ok = len(posts) == 1
    q = parse_qs(posts[0][2].decode()) if ok else {}
    step("the POST carries the 3 checked tiles + the submit button, urlencoded", ok and len([k for k in q if k.startswith("image-check_")]) == 3 and "challenge-submit" in q and
         posts[0][1].get("Content-Type", "").startswith("application/x-www-form-urlencoded"), str(list(q)[:4]))
    step("the 302 answer is followed with a GET", any(g.startswith("/html/after.html") for g in gets))
    log = vm.serial()
    bad = [l for l in log.splitlines() if "kmalloc FAIL" in l or "bad or double free" in l or "PAGE FAULT" in l.upper()]
    step("no faults", not bad, str(bad[:3]))
    print("RESULT:", "PASS" if all(ok for _, ok in results) else "FAIL")
finally:
    if vm:
        vm.kill()
    srv.shutdown()
sys.exit(0 if all(ok for _, ok in results) else 1)
