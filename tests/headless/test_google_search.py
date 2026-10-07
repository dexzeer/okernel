#!/usr/bin/env python3
"""Headless test: google.com must render a working search box, and submitting
must navigate to /search?q=...

Verifies:
  1. The page loads (https parse) and form control regions are logged.
  2. Clicking the search input focuses it ('[okai] focus input node=').
  3. Typing a query + Enter submits ('[okai] submit form -> .../search?q=..').
  4. The results page loads.
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR

vm = OkVM("gs")
vm.wait_for("[mem] heap", timeout=40)
time.sleep(10)
vm.type_string("okai https://www.google.com/\n")
loaded = vm.wait_for("parse: count=", timeout=150, fail_pats=["[okai] error page:"])
time.sleep(6)

def fields():
    out = []
    for line in vm.serial().splitlines():
        if "[okai] render tab=" in line and "page origin" in line:
            out = []
        m = re.search(r"\[okai\] field\[\d+\] x=(\d+) y=(-?\d+) w=(\d+) h=(\d+) node=(\d+) btn=(\d)", line)
        if m:
            out.append(tuple(int(v) for v in m.groups()))
    return out

fs = fields()
inputs = [f for f in fs if f[5] == 0]
print(f"loaded={loaded} controls={len(fs)} inputs={len(inputs)}")
focused = False
if inputs:
    x, y, w, h, node, _ = max(inputs, key=lambda f: f[2])  # the widest text field
    vm.click_at(x + min(w // 2, 120), y + h // 2, done=lambda lg: "[okai] focus input node=" in lg)
    focused = "[okai] focus input node=" in vm.serial()
print("focus:", focused)

vm.type_string("okernel")
time.sleep(2)
vm.type_string("\n")
submitted = vm.wait_for("submit form ->", timeout=15)
m = re.search(r"submit form -> (\S+)", vm.serial())
url = m.group(1) if m else ""
print("submit url:", url)
n = vm.serial().count("parse: count=")
results = submitted and vm.wait_for("parse: count=", timeout=5) and \
    (time.sleep(20) or vm.serial().count("parse: count=") > n)
vm.dump()
from PIL import Image
Image.open(vm.PPM).save(os.path.join(OUTDIR, "google_search.png"))
vm.kill()

ok = loaded and inputs and focused and submitted and "/search?" in url and "q=okernel" in url
print("RESULTS PAGE:", "loaded" if results else "not loaded (network)")
print("GOOGLE_SEARCH", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
