#!/usr/bin/env python3
"""A <video src> on another origin plays without CORS headers (offline, local servers on :8004 and :8007).

Invidious (quality=medium) points <video src> at its companion, which 302-redirects to googlevideo; googlevideo sends no
Access-Control-Allow-Origin. A browser loads a media element without a crossorigin attribute as a no-cors request, so it
plays. okai used to read it through the page's CORS-checked fetch() and threw the bytes away ("CORS: blocked").

Checks: the plain element plays to the end through a cross-origin redirect; a second element WITH crossorigin fails with a
network error; a script fetch() of the same URL is still blocked (the media path does not open a hole for page scripts).
"""
import sys, os, time, re, shutil, threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VEC = os.path.join(ROOT, "tests", "media", "vec", "webm_av.webm")
if not os.path.exists(VEC):
    print("SKIP  webm_av.webm missing (run tests/media/gen_webm_vectors.py)")
    sys.exit(0)
WWW = os.path.join(OUTDIR, "www-nocors")
shutil.rmtree(WWW, ignore_errors=True)
os.makedirs(WWW)
DATA = open(VEC, "rb").read()
CLIP = "http://10.0.2.2:8007/redirect/clip.webm"
html = open(os.path.join(ROOT, "tests", "web", "js", "media.html")).read().replace("../../media/vec/webm_av.webm", CLIP)
# a second element WITH crossorigin (a CORS load: must fail) and a script fetch() of the same URL (must stay blocked)
probe = ("<video id=\"x\" crossorigin=\"anonymous\" src=\"%s?xo\" muted style=\"width:32px;height:18px\"></video>\n"
         "<script>document.getElementById('x').addEventListener('error', e => console.log('XO error code=' + e.target.error.code));\n"
         "document.getElementById('x').addEventListener('loadedmetadata', () => console.log('XO loadedmetadata'));\n"
         "fetch('%s', { headers: { Range: 'bytes=0-99' } }).then(r => console.log('FETCH readable status=' + r.status),"
         " e => console.log('FETCH blocked ' + e.name));</script>\n</body>") % (CLIP, CLIP)
open(os.path.join(WWW, "plain.html"), "w").write(html.replace("</body>", probe))
media_hits = []


class Page(SimpleHTTPRequestHandler):           # :8004, the document's origin
    def __init__(self, *a, **k):
        super().__init__(*a, directory=WWW, **k)

    def log_message(self, *a):
        pass


class Media(SimpleHTTPRequestHandler):          # :8007, another origin: redirect, then 206 without any CORS header
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/redirect/"):
            self.send_response(302)
            self.send_header("Location", "/clip.webm")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        m = re.match(r"bytes=(\d+)-(\d*)", self.headers.get("Range") or "bytes=0-")
        a = int(m.group(1))
        b = min(int(m.group(2)) if m.group(2) else len(DATA) - 1, len(DATA) - 1)
        if a >= len(DATA):
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % len(DATA))
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        media_hits.append((a, b))
        body = DATA[a:b + 1]
        self.send_response(206)
        self.send_header("Content-Type", "video/webm")
        self.send_header("Content-Range", "bytes %d-%d/%d" % (a, b, len(DATA)))
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


srvs = [ThreadingHTTPServer(("0.0.0.0", 8004), Page), ThreadingHTTPServer(("0.0.0.0", 8007), Media)]
for s in srvs:
    threading.Thread(target=s.serve_forever, daemon=True).start()
results = []


def step(name, ok):
    results.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name, flush=True)


def open_url(vm, url):
    n = vm.serial().count("parse: count=")
    for attempt in range(4):
        vm.type_string(url + "\n", delay=0.05)
        time.sleep(4)
        if vm.serial().count("parse: count=") > n:
            return
        vm.type_string("\n", delay=0.05)
        time.sleep(2)


vm = OkVM(sys.argv[1] if len(sys.argv) > 1 else "okainocors")
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(10)
    vm.type_string("cpu slow 1\n", delay=0.05)
    time.sleep(1)
    open_url(vm, "okai http://10.0.2.2:8004/plain.html")
    step("cross-origin <video src> without CORS headers plays to the end", vm.wait_for("RESULT dur=", timeout=150))
    step("script fetch() of the same URL is still blocked", vm.wait_for("FETCH blocked", timeout=20) and "FETCH readable" not in vm.serial())
    step("the crossorigin element fails with a network error", vm.wait_for("XO error code=2", timeout=30) and "XO loadedmetadata" not in vm.serial())
    serial = vm.serial()
    blocked = [l for l in serial.splitlines() if "CORS: blocked" in l]
    # exactly the fetch() and the crossorigin element's first range: the plain element is never blocked
    step("CORS blocks: %d (fetch + crossorigin element only)" % len(blocked),
         len(blocked) == 2 and sum(1 for l in blocked if l.rstrip().endswith("?xo")) == 1)
    step("the media server was read (%d ranges)" % len(media_hits), len(media_hits) >= 1)
    bad = [l for l in serial.splitlines() if "kmalloc FAIL" in l or "PAGE FAULT" in l.upper()]
    step("no faults", not bad)
finally:
    vm.kill()
    for s in srvs:
        s.shutdown()
ok = all(r for _, r in results)
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
