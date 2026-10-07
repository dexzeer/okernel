#!/usr/bin/env python3
"""okai interaction test (offline: local fixture server on the host, :8000).

  1. `okai http://10.0.2.2:8000/index.html` loads + renders (CSS + image sub-resources)
  2. clicking the checkbox toggles it
  3. closed-loop click on a link -> page 2 loads (LINK HIT + parse)
  4. 'b' goes back -> index parses again
  5. Tab focuses the first form field, typing edits it, Enter submits a GET
     with the typed value (kernel log + the server's access log agree)
  6. the mouse wheel scrolls
PASS requires all steps and no faults / allocator errors.
"""
import sys, os, time, re, subprocess, zlib, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR

WWW = os.path.join(OUTDIR, "www-okai")
os.makedirs(WWW, exist_ok=True)
open(os.path.join(WWW, "index.html"), "w").write("""<!doctype html>
<html><head><title>okai fixture</title><link rel=stylesheet href=style.css></head>
<body><h1>Fixture index</h1>
<p>Go to <a id=p2 href="page2.html">the second page</a> or stay.</p>
<form action="search" method=get>
  <input name=q placeholder="type here"> <label><input type=checkbox name=c value=yes> check</label>
  <button type=submit>Go</button>
</form>
<img src="dot.png" width=40 height=40 alt=dot>
<div style="height:3000px;background:linear-gradient(#fff,#4a8)">tall</div>
</body></html>""")
open(os.path.join(WWW, "style.css"), "w").write(
    "body{font-family:sans-serif;margin:24px;background:#f4f6fb}h1{color:#2a4f9e}"
    "a{color:#c03;font-weight:bold}input{font-size:18px}")
open(os.path.join(WWW, "page2.html"), "w").write(
    "<!doctype html><title>page two</title><h1>Second page</h1><p><a href=index.html>back</a>")
open(os.path.join(WWW, "search"), "w").write("<!doctype html><title>results</title><p>results")


def png(w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


open(os.path.join(WWW, "dot.png"), "wb").write(png(2, 2, (220, 40, 40)))

SRVLOG = os.path.join(OUTDIR, "www-okai.log")
srvlog = open(SRVLOG, "w")
srv = subprocess.Popen([sys.executable, "-m", "http.server", "8000", "--directory", WWW],
                       stdout=srvlog, stderr=subprocess.STDOUT)
time.sleep(1)
results = []


def step(name, ok):
    results.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name, flush=True)


def wait_count(vm, pat, n, timeout=20):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if vm.serial().count(pat) > n:
            return True
        time.sleep(1)
    return False


vm = None
try:
    vm = OkVM("interact")
    vm.wait_for("[mem] heap", timeout=40)
    time.sleep(10)
    vm.type_string("okai http://10.0.2.2:8000/index.html\n")
    ok = vm.wait_for("sub-res done", timeout=60)
    time.sleep(2)
    log = vm.serial()
    step("index loads with css + image",
         ok and "sub-res CSS: status=200" in log and "sub-res IMG: status=200" in log)

    # checkbox: the smallest button-kind control region
    fields = [tuple(int(v) for v in m.groups()) for m in re.finditer(
        r"\[okai\] field\[\d+\] x=(\d+) y=(\d+) w=(\d+) h=(\d+) node=\d+ btn=1", vm.serial())]
    box = min(fields, key=lambda f: f[2] * f[3]) if fields else None
    if box:
        vm.click_at(box[0] + box[2] // 2, box[1] + box[3] // 2,
                    done=lambda lg: "[okai] toggle node=" in lg)
    step("checkbox click toggles", vm.wait_for("checked=1", timeout=5))

    n0 = vm.serial().count("parse: count=")
    hit = vm.click_link("page2.html")
    step("link click loads page 2", hit and wait_count(vm, "parse: count=", n0))

    n1 = vm.serial().count("parse: count=")
    time.sleep(2)
    vm.type_string("b")
    back = wait_count(vm, "parse: count=", n1)
    time.sleep(3)
    navs = re.findall(r"\[okai\] navigate (\S+)", vm.serial())
    step("back navigates to index", back and navs and navs[-1].endswith("index.html"))

    vm.type_string("\t")
    time.sleep(1.5)
    focus = "[okai] tab focus node=" in vm.serial()
    vm.type_string("hello")
    time.sleep(2)
    vm.type_string("\n")
    sub = vm.wait_for("submit form ->", timeout=10)
    time.sleep(4)
    m = re.search(r"submit form -> (\S+)", vm.serial())
    url = m.group(1) if m else ""
    srvlog.flush()
    access = open(SRVLOG).read()
    step("tab focus + typing + enter submits q=hello",
         focus and sub and url.endswith("/search?q=hello") and "GET /search?q=hello" in access)

    n2 = vm.serial().count("parse: count=")
    vm.type_string("b")
    wait_count(vm, "parse: count=", n2)
    time.sleep(2)
    for _ in range(4):
        vm.mon("mouse_move 0 0 -1", 0.3)  # QEMU dz<0 = wheel down
    time.sleep(2)
    wl = re.findall(r"\[okai\] wheel dy=(-?\d+) old_scroll=(\d+)", vm.serial())
    step("wheel scrolls down", len(wl) >= 2 and int(wl[-1][1]) > 0)
    vm.dump(os.path.join(OUTDIR, "interact.ppm"))

    log = vm.serial()
    bad = [l for l in log.splitlines()
           if "kmalloc FAIL" in l or "bad or double free" in l or
           ("EXCEPTION" in l.upper() and "#" in l) or "PAGE FAULT" in l.upper()]
    print("bad lines:", bad[:5])
    print("RESULT:", "PASS" if all(ok for _, ok in results) and not bad else "FAIL")
finally:
    if vm:
        vm.kill()
    srv.terminate()
