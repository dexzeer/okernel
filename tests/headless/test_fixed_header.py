#!/usr/bin/env python3
"""QEMU end-to-end: CSS position:fixed header stays pinned while scrolling.

Mouse-free. Serves two pages over plain HTTP (guest reaches the host at
10.0.2.2): control.html (150 body lines) and pos.html (same body + a
3-row fixed header: blue background, red third row for tracking).

Asserts:
 1. scrolling with 'j' moves body pixels (keyboard focus works),
 2. the header's blue/red rows are IDENTICAL before/after scroll (pinned),
 3. rows below the header still change (body scrolls under it),
 4. no crash.

Known environment quirks (not failures): a few 'j' keystrokes can be
dropped by sendkey timing (scrolls fewer rows — still detected), and the
header's first row tucks under the 96px pixel chrome (pre-existing
chrome/content overlap affecting every page's row 0, so tracking uses
rows 1+).

Self-contained: writes the fixtures to ~/okvm/www-fixhdr and serves them on
:18080 (QEMU user-net must be up).
"""
import sys, os, time, re, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR

WWW = os.path.join(OUTDIR, "www-fixhdr")
os.makedirs(WWW, exist_ok=True)
BODY = "".join("<div>Body line %d of the scrolling page</div>" % i for i in range(150))
open(os.path.join(WWW, "control.html"), "w").write(
    "<!doctype html><title>control</title><body style='font-size:20px'>" + BODY)
open(os.path.join(WWW, "pos.html"), "w").write(
    "<!doctype html><title>pos</title><body style='font-size:20px;padding-top:96px'>"
    "<div style='position:fixed;top:0;left:0;right:0;background:#0000cc;color:#fff'>"
    "<div style='height:32px'>header row 1</div><div style='height:32px'>header row 2</div>"
    "<div style='height:32px;background:#cc0000'>header row 3</div></div>" + BODY)
SRV = subprocess.Popen([sys.executable, "-m", "http.server", "18080", "--directory", WWW],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)

BASE = "http://10.0.2.2:18080/"
ROW_THRESH = 10


def loadppm(path):
    d = open(os.path.expanduser(path), 'rb').read()
    parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def frame_diff(a, b):
    aw, ah, apx = a
    bw, bh, bpx = b
    assert (aw, ah) == (bw, bh) and len(apx) == len(bpx)
    stride = len(apx) // ah
    per_row = []
    for r in range(ah):
        ra = apx[r*stride:(r+1)*stride]
        rb = bpx[r*stride:(r+1)*stride]
        per_row.append(sum(1 for x, y in zip(ra, rb) if x != y))
    return per_row


def color_rows(path, pred, min_px=100):
    """Framebuffer rows where >min_px pixels satisfy pred(R, G, B)."""
    w, h, px = loadppm(path)
    st = len(px) // h
    ch = w * 3
    out = []
    for r in range(h):
        row = px[r*st:(r+1)*st]
        n = sum(1 for i in range(0, ch, 3) if pred(row[i], row[i+1], row[i+2]))
        if n > min_px:
            out.append(r)
    return out


def is_blue(R, G, B):
    return B > 150 and R < 100 and G < 100


def is_red(R, G, B):
    return R > 150 and G < 100 and B < 100


def load(vm, page):
    n = vm.serial().count("parse: count=")
    vm.type_string("g")  # focus address bar
    time.sleep(0.7)
    vm.type_string(BASE + page + "\n")
    t0 = time.time()
    while time.time() - t0 < 90:
        if vm.serial().count("parse: count=") > n:
            time.sleep(2)
            return True
        time.sleep(1)
    return False


def scroll12(vm):
    for _ in range(12):
        vm.type_string("j")
        time.sleep(0.35)
    time.sleep(1.0)


vm = OkVM("fixhdr")
time.sleep(14)
vm.type_string("okai\n")  # open browser window first (shell has focus)
time.sleep(3)

ok = True
print("load control:", load(vm, "control.html"))
time.sleep(2)
vm.dump(os.path.expanduser("~/okvm/fixhdr_c0.ppm"))
c0 = loadppm("~/okvm/fixhdr_c0.ppm")
scroll12(vm)
vm.dump(os.path.expanduser("~/okvm/fixhdr_c1.ppm"))
c1 = loadppm("~/okvm/fixhdr_c1.ppm")

print("load pos:", load(vm, "pos.html"))
time.sleep(2)
vm.dump(os.path.expanduser("~/okvm/fixhdr_h0.ppm"))
h0 = loadppm("~/okvm/fixhdr_h0.ppm")
scroll12(vm)
vm.dump(os.path.expanduser("~/okvm/fixhdr_h1.ppm"))
h1 = loadppm("~/okvm/fixhdr_h1.ppm")

dc = frame_diff(c0, c1)
dh = frame_diff(h0, h1)
scrolled_ctrl = sum(1 for n in dc if n > ROW_THRESH)
scrolled_hdr = sum(1 for n in dh if n > ROW_THRESH)
print(f"control changed rows: {scrolled_ctrl}, header-page changed rows: {scrolled_hdr}")
if scrolled_ctrl < 50:
    print("FAIL: body did not scroll (keyboard focus?)")
    ok = False

blue0 = [r for r in color_rows("~/okvm/fixhdr_h0.ppm", is_blue) if r >= 60]
blue1 = [r for r in color_rows("~/okvm/fixhdr_h1.ppm", is_blue) if r >= 60]
red0 = [r for r in color_rows("~/okvm/fixhdr_h0.ppm", is_red) if r >= 60]
red1 = [r for r in color_rows("~/okvm/fixhdr_h1.ppm", is_red) if r >= 60]
print(f"blue rows: {len(blue0)} -> {len(blue1)}, red rows: {len(red0)} -> {len(red1)}")
pinned = (len(blue0) >= 24 and blue0 == blue1 and len(red0) >= 10 and red0 == red1)
print("header pinned (identical color rows across scroll):", pinned)
if not pinned:
    print("FAIL: fixed header moved with the scroll")
    ok = False

if blue0:
    below = [n for r, n in enumerate(dh) if r > blue0[-1] + 24 and n > ROW_THRESH]
    print(f"changed rows below header: {len(below)}")
    if len(below) < 10:
        print("FAIL: body did not scroll under the header")
        ok = False

crash = "triple fault" in vm.serial()
print("crash:", crash)
vm.kill()
SRV.terminate()
if ok and not crash:
    print("FIXHDR PASS")
    sys.exit(0)
print("FIXHDR FAIL")
sys.exit(1)
