#!/usr/bin/env python3
"""Headless regression: failure paths must not wedge the browser (HANDOFF.md
bug #22). Every failed fetch must (a) render the 'Unable to load page' error
and (b) RELEASE the fetch owner so the next navigation still works."""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

def parses(log):
    return len(re.findall(r"\[br\] (?:https )?parse: count=\d+", log))

vm = OkVM("err")
time.sleep(14)
results = []

# TEST 1: baseline page loads
vm.type_string("okai https://example.com/\n")
results.append(("baseline loads", vm.wait_for("parse: count=", timeout=70)))
time.sleep(2)

# TEST 2: malformed URL (empty host) refused, error page rendered, owner freed
vm.type_string("g")               # focus the okai address bar
time.sleep(0.5)
vm.type_string("/domains/example\n")
refused = vm.wait_for("refusing fetch: empty host", timeout=20)
vm.dump()
from PIL import Image
img = Image.open(vm.PPM).convert('RGB')
px = img.load()
err_red = False
for y in range(60, 400):          # error title is light-red (VGA 12)
    hits = sum(1 for x in range(1015, 1880)   # okai window moved right of terminal
               if px[x, y][0] > 200 and px[x, y][1] < 120 and px[x, y][2] < 120)
    if hits > 30: err_red = True; break
results.append(("empty-host refused", refused))
results.append(("error page rendered", err_red))

# TEST 3: recovery — a good URL must STILL load after the refusal
n0 = parses(vm.serial())
vm.type_string("g"); time.sleep(0.5)
vm.type_string("example.com\n")
results.append(("recovery after refusal",
                vm.wait_for("parse: count=", timeout=70) and parses(vm.serial()) > n0))
time.sleep(2)

# TEST 4: NXDOMAIN — DNS failure aborts cleanly (no infinite wedge)
n1 = parses(vm.serial())
vm.type_string("g"); time.sleep(0.5)
vm.type_string("nonexistent.invalid\n")
results.append(("NXDOMAIN handled",
                vm.wait_for("query error, rcode=3", timeout=30)))

# TEST 5: recovery after NXDOMAIN (this is where the old build wedged forever)
n2 = parses(vm.serial())
vm.type_string("g"); time.sleep(0.5)
vm.type_string("example.com\n")
ok5 = vm.wait_for("parse: count=", timeout=70) and parses(vm.serial()) > n2
results.append(("recovery after NXDOMAIN", ok5))

vm.kill()
all_ok = True
for name, ok in results:
    print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    all_ok &= ok
print("\nVERDICT:", "PASS" if all_ok else "FAIL")
sys.exit(0 if all_ok else 1)
