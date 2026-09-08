#!/usr/bin/env python3
"""Headless test: google.com must render a working search box and submitting
must navigate to /search?q=... (the 'make it searchable' fix).

Verifies:
  1. Page loads (https parse: count=) and form field regions are logged.
  2. Screenshot (after scrolling the box into view) shows a search input.
  3. Clicking the search input focuses it (serial '[okai] focus input token=').
  4. Typing a query + Enter submits (serial '[okai] submit form_idx=.. -> .../search?q=..').
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

OUTDIR = os.path.expanduser("~/okvm")
PNG = os.path.join(OUTDIR, "google_search.png")

vm = OkVM("gs")
time.sleep(14)

vm.type_string("okai https://www.google.com/\n")

loaded = vm.wait_for("https parse: count=", timeout=150,
                     fail_pats=["FAILED", "giving up", "timed out"])
log = vm.serial()

def parse_fields():
    out = {}
    for line in vm.serial().splitlines():
        fm = re.search(r"\[okai\] field\[(\d+)\] row=(-?\d+) col0=(\d+) col1=(\d+) btn=(\d+) tok=(\d+)", line)
        if fm:
            i = int(fm.group(1))
            out[i] = {"row": int(fm.group(2)), "col0": int(fm.group(3)),
                      "col1": int(fm.group(4)), "btn": int(fm.group(5)),
                      "tok": int(fm.group(6))}
    return out

m = re.search(r"\[br\] https parse: count=(\d+)", log)
count = int(m.group(1)) if m else 0
print(f"loaded={loaded} tokens={count}")

fields = parse_fields()
inputs = [i for i, f in fields.items() if f["btn"] == 0]
fields_logged = len(fields) > 0
print(f"field count: {len(fields)}, inputs: {inputs}")
for i, f in sorted(fields.items()):
    print("  ", i, f)

# ---- Scroll the search box into view (fields start off-screen, row=-1) ----
def input_row():
    fs = parse_fields()
    for i, f in fs.items():
        if f["btn"] == 0:
            return f["row"]
    return None

ON_SCREEN = False
for _ in range(40):
    r = input_row()
    if r is not None and r >= 0:
        ON_SCREEN = True
        break
    vm.type_string("j")   # scroll down one line
    time.sleep(0.25)
print(f"search box on-screen after scrolling: {ON_SCREEN} (row={input_row()})")

# ---- Screenshot for vision check (now that the box should be visible) ----
w, h, px = vm.dump()
try:
    from PIL import Image
    Image.open(vm.PPM).save(PNG)
except Exception as e:
    print("ppm->png failed:", e); PNG = vm.PPM
print(f"screenshot: {PNG}")

# ---- Pick the on-screen search INPUT ----
fs = parse_fields()
tgt = None
for i, f in fs.items():
    if f["btn"] == 0 and f["row"] >= 0:
        tgt = f; break
if not tgt:
    print("FAIL: search input not on screen"); vm.kill(); sys.exit(1)

brow, col0, col1 = tgt["row"], tgt["col0"], tgt["col1"]
tcol = (col0 + col1) // 2
print(f"target input field row={brow} cols {col0}..{col1} (center {tcol})")

# ---- Google window geometry: window at (1010,60) no_titlebar; verified live
# as cw=12.29 chh=24.08 (CONTENT_GW/font_scale, CONTENT_GH/font_scale). ----
OX, OY = 1012, 62            # content-grid origin (x+WIN_BORDER, y+WIN_BORDER)
CW, CH = 12, 24
def tx(col): return OX + col * CW + CW // 2
def ty(row): return OY + row * CH + CH // 2

def all_clicks():
    out = []
    for l in vm.serial().splitlines():
        mm = re.search(r"row=(\d+) col=(\d+) \(mx=(\d+) my=(\d+)\)", l)
        if mm: out.append((int(mm.group(1)), int(mm.group(2)),
                           int(mm.group(3)), int(mm.group(4))))
    return out

focused = False
tx0, ty0 = tx(tcol), ty(brow)
# (1) Force the mouse to the top-left corner so its position is known, then
# walk it into the browser window (relative bursts defeat the kernel's
# 4-sample PS/2 smoothing ring).
for _ in range(12):
    vm.burst(-300, -300)
# (2) Converge onto the input field: each registered click reports its true
# (row,col,mx,my); correct the mouse toward the field until a focus fires.
# The walk uses FINE bursts so discrete click positions actually sample the
# window interior (coarse bursts jump straight past it and never register).
last_mx = last_my = None   # last kernel-reported mouse pos (ground truth)
for attempt in range(200):
    before = len(all_clicks())
    vm.click()
    time.sleep(0.3)
    if "[okai] focus input token=" in vm.serial():
        focused = True
        print(f"focused input after {attempt+1} tries")
        break
    cl = all_clicks()
    if len(cl) > before:           # a content click registered -> update ground truth
        last_mx, last_my = cl[-1][2], cl[-1][3]
    if last_mx is not None:
        # Converge toward the field using the kernel's own reported position.
        # Burst delta is divided by ~4.5 (the PS/2 smoothing-ring amplification)
        # and CAPPED at +-12 so a single step can never overshoot out of the
        # window; the loop re-reads and re-corrects every iteration.
        dx, dy = tx0 - last_mx, ty0 - last_my
        bx = max(-12, min(12, int(dx / 4.5)))
        by = max(-12, min(12, int(dy / 4.5)))
        if bx == 0 and abs(dx) > 4: bx = 1 if dx > 0 else -1
        if by == 0 and abs(dy) > 4: by = 1 if dy > 0 else -1
        vm.burst(bx, by)
    else:
        vm.burst(10, 6)            # not yet in-window; walk in with fine steps
print("focused:", focused)

BOX_PNG = os.path.join(OUTDIR, "google_box.png")
RES_PNG = os.path.join(OUTDIR, "google_results.png")

if focused:
    vm.type_string("okai test query")
    time.sleep(0.5)
    # Screenshot the focused box with typed text (for vision verification).
    w, h, px = vm.dump()
    try:
        from PIL import Image
        Image.open(vm.PPM).save(BOX_PNG)
    except Exception as e:
        print("ppm->png failed:", e); BOX_PNG = vm.PPM
    print(f"box screenshot: {BOX_PNG}")
    vm.type_string("\n")
    time.sleep(2.0)
    # Screenshot the results page (for vision verification).
    w, h, px = vm.dump()
    try:
        from PIL import Image
        Image.open(vm.PPM).save(RES_PNG)
    except Exception as e:
        print("ppm->png failed:", e); RES_PNG = vm.PPM
    print(f"results screenshot: {RES_PNG}")

sm = re.search(r"\[okai\] submit form_idx=\d+ -> (\S+)", vm.serial())
submit_url = sm.group(1) if sm else ""
# Google puts other params (ie=, hl=, source=) before q=, so check for the
# submit line plus the query echo rather than the literal "search?q=".
submit_ok = ("[okai] submit form_idx=" in vm.serial()
             and "q=" in submit_url and "okai" in submit_url)
print(f"submit_ok={submit_ok} url={submit_url}")

vm.kill()
verdict = loaded and count >= 40 and fields_logged and ON_SCREEN and focused and submit_ok
print("\nVERDICT:", "PASS" if verdict else "FAIL")
sys.exit(0 if verdict else 1)
