#!/usr/bin/env python3
"""okai <canvas> 2D test (offline, local fixture server on :8003): the kernel's rendering of the canvas
scenes (tests/web/js/canvas_scene*.html) must match the host rendering of the same engine (wbrowse), and
an animated canvas (requestAnimationFrame) must keep changing on screen.  Run in WSL.
"""
import sys, os, time, re, subprocess, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WWW = os.path.join(OUTDIR, "www-canvas")
shutil.rmtree(WWW, ignore_errors=True)
os.makedirs(WWW)
PAGES = [("canvas_scene.html", 400, 300), ("canvas_scene2.html", 500, 360)]
for p in [p for p, _, _ in PAGES] + ["canvas_anim.html"]:
    shutil.copy(os.path.join(ROOT, "tests", "web", "js", p), os.path.join(WWW, p))
srv = subprocess.Popen([sys.executable, "-m", "http.server", "8003", "--directory", WWW], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
time.sleep(1)
results = []

def step(name, ok):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name, flush=True)

def host_render(page, w, h):
    ppm = os.path.join(OUTDIR, page + ".host.ppm")
    subprocess.run([os.path.join(ROOT, "build-host", "wbrowse"), os.path.join(ROOT, "tests", "web", "js", page), str(w), str(h), ppm, "5"],
                   capture_output=True, timeout=120)
    data = open(ppm, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    pw, ph = int(m.group(1)), int(m.group(2))
    return np.frombuffer(data[m.end():], dtype=np.uint8).reshape(ph, pw, 3).astype(np.int32)

def origin(log):
    ms = re.findall(r"\(page origin (\d+),(\d+)\)", log)
    return (int(ms[-1][0]), int(ms[-1][1])) if ms else (0, 0)

vm = OkVM("okaicanvas")
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(10)
    base = "http://10.0.2.2:8003/"
    first = True
    for page, w, h in PAGES + [("canvas_anim.html", 300, 200)]:
        if not first:
            vm.type_string("g"); time.sleep(0.5)
        for attempt in range(4):
            vm.type_string(("okai " if first else "") + base + page + "\n", delay=0.05)
            time.sleep(4)
            if "[okai] fetch " + base + page in vm.serial(): break
            print("  (command garbled, retrying)"); vm.type_string("\n", delay=0.05); time.sleep(2)
        first = False
        n = vm.serial().count("parse: count=")
        if page != "canvas_anim.html":
            step(page + ": script ran", vm.wait_for("RESULT ", timeout=90))
            time.sleep(3)
            ox, oy = origin(vm.serial())
            sw, sh, pix = vm.dump()
            full = np.frombuffer(pix, dtype=np.uint8).reshape(sh, sw, 3).astype(np.int32)
            ours = full[oy:oy + h, ox:ox + w]
            ref = host_render(page, w, h)[:h, :w]
            d = np.abs(ours - ref)
            step(f"{page}: kernel render matches the host render (mean abs diff {d.mean():.3f}, >32: {(d.max(axis=2) > 32).sum()} px)", d.mean() < 2.0)
        else:
            time.sleep(3)
            ox, oy = origin(vm.serial())
            sw, sh, pix = vm.dump(); a = np.frombuffer(pix, dtype=np.uint8).reshape(sh, sw, 3)[oy:oy + 200, ox:ox + 300].astype(np.int32)
            time.sleep(0.6)
            sw, sh, pix = vm.dump(); b = np.frombuffer(pix, dtype=np.uint8).reshape(sh, sw, 3)[oy:oy + 200, ox:ox + 300].astype(np.int32)
            step(f"animation moves on screen (mean abs diff {np.abs(a - b).mean():.2f}, orange in shot: {int(((a[..., 0] > 200) & (a[..., 2] < 60)).sum())} px)",
                 np.abs(a - b).mean() > 0.2 and ((a[..., 0] > 200) & (a[..., 2] < 60)).sum() > 500)
    log = vm.serial()
    bad = [l for l in log.splitlines() if "kmalloc FAIL" in l or "bad or double free" in l or "PAGE FAULT" in l.upper() or "abort()" in l]
    step("no faults / allocator errors", not bad)
    errs = [l for l in log.splitlines() if l.startswith("[js] error")]
    print("js errors:", errs[:5])
    step("no script errors", not errs)
finally:
    vm.kill(); srv.terminate()
print("RESULT:", "PASS" if results and all(results) else "FAIL")
sys.exit(0 if results and all(results) else 1)
