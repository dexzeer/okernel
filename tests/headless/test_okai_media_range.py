#!/usr/bin/env python3
"""<video src> is streamed with Range requests (offline, local fixture server on :8004 that honours Range).

The 10 s clip is fetched in 64 KB pieces (window.__mediaChunk, a test knob of the media element): it must start playing
before the file is complete, play to the end with every frame, and the server must have seen several 206 answers.
"""
import sys, os, time, re, shutil, threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VEC = os.path.join(ROOT, "tests", "media", "vec", "webm_av_long.webm")
if not os.path.exists(VEC):
    print("SKIP  webm_av_long.webm missing (run tests/media/gen_webm_vectors.py)")
    sys.exit(0)
WWW = os.path.join(OUTDIR, "www-range")
shutil.rmtree(WWW, ignore_errors=True)
os.makedirs(WWW)
shutil.copy(VEC, os.path.join(WWW, "clip.webm"))
html = open(os.path.join(ROOT, "tests", "web", "js", "media.html")).read().replace("../../media/vec/webm_av.webm", "clip.webm")
open(os.path.join(WWW, "range.html"), "w").write(html.replace("<script>", "<script>window.__mediaChunk = 65536;\n", 1))

log = []


class H(SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=WWW, **k)

    def log_message(self, *a):
        pass

    def do_GET(self):
        rng = self.headers.get("Range")
        if not self.path.endswith("clip.webm") or not rng:
            return super().do_GET()
        data = open(os.path.join(WWW, "clip.webm"), "rb").read()
        m = re.match(r"bytes=(\d+)-(\d*)", rng)
        a = int(m.group(1))
        b = min(int(m.group(2)) if m.group(2) else len(data) - 1, len(data) - 1)
        if a >= len(data):
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % len(data))
            self.end_headers()
            return
        log.append((a, b))
        body = data[a:b + 1]
        self.send_response(206)
        self.send_header("Content-Type", "video/webm")
        self.send_header("Content-Range", "bytes %d-%d/%d" % (a, b, len(data)))
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        self.wfile.write(body)


srv = ThreadingHTTPServer(("0.0.0.0", 8004), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
results = []


def step(name, ok):
    results.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name, flush=True)


vm = OkVM(sys.argv[1] if len(sys.argv) > 1 else "okairange")
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(10)
    vm.type_string("cpu slow 1\n", delay=0.05)
    time.sleep(1)
    want = "okai http://10.0.2.2:8004/range.html"
    for attempt in range(4):
        vm.type_string(want + "\n", delay=0.05)
        time.sleep(4)
        if "[okai] fetch http://10.0.2.2:8004/range.html" in vm.serial():
            break
        vm.type_string("\n", delay=0.05)
        time.sleep(2)
    step("loadedmetadata", vm.wait_for("EV loadedmetadata", timeout=90))
    step("playing starts", vm.wait_for("EV playing", timeout=60))
    step("ended", vm.wait_for("EV ended", timeout=90))
    step("every frame decoded", vm.wait_for("size=320x180 frames=300 ", timeout=20))
    size = os.path.getsize(os.path.join(WWW, "clip.webm"))
    step("the server saw %d range requests of <= 64 KB covering the file" % len(log),
         len(log) >= size // 65536 and max(b - a + 1 for a, b in log) <= 65536 and max(b for a, b in log) == size - 1)
    serial = vm.serial()
    bad = [l for l in serial.splitlines() if "kmalloc FAIL" in l or "PAGE FAULT" in l.upper()]
    step("no faults", not bad)
finally:
    vm.kill()
    srv.shutdown()
ok = all(r for _, r in results)
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
