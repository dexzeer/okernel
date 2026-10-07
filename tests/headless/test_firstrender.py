#!/usr/bin/env python3
"""Headless render regression: the first website must parse, lay out, and be
navigable in the real guest (linked <li> bullets, uppercase </A>, DT/DD,
whitespace collapse, wrapped links). Page 2 goes through the address bar
(keyboard = geometry-free); link clicking is covered by test_links.py /
test_okai_interact.py.

Verdict from serial ground truth (`[okai] link[i] x= y= w= h= href=` regions
of each page's first render) + screenshots for vision.
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

HOME_HREFS = {
    "http://info.cern.ch/hypertext/WWW/TheProject.html",
    "http://line-mode.cern.ch/www/hypertext/WWW/TheProject.html",
    "http://home.web.cern.ch/topics/birth-web",
    "http://home.web.cern.ch/about",
}
PROJECT_MUST = ["WhatIs.html", "Help.html", "History.html", "Bibliography.html"]
P2_URL = "http://info.cern.ch/hypertext/WWW/TheProject.html"

fails = []
def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name, extra, flush=True)
    if not cond:
        fails.append(name)

def wait_parses(vm, before, timeout=150):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if len(re.findall(r"parse: count=\d+", vm.serial())) > before:
            time.sleep(3)  # let the first render log its link regions
            return True
        time.sleep(1)
    return False

vm = OkVM("fr")
vm.wait_for("[mem] heap", timeout=40)
time.sleep(10)

# ---- page 1: the home page over plain HTTP ----
vm.type_string("okai http://info.cern.ch/\n")
ok1 = wait_parses(vm, 0)
check("home fetched+parsed", ok1)
m = re.search(r"parse: count=(\d+)", vm.serial())
n1 = int(m.group(1)) if m else 0
check("home node count sane (>=20)", n1 >= 20, "(count=%d)" % n1)

links = vm.link_regions()
hrefs = {r[5] for r in links}
check("home has 4+ link regions", len(links) >= 4, "(%d)" % len(links))
missing = {h for h in HOME_HREFS if h not in hrefs}
check("home hrefs exact", not missing, "(missing=%s)" % (missing,))
bad = [r for r in links if r[3] < 8 or r[4] < 8]
check("no degenerate link regions", not bad, "(%s)" % (bad[:3],))
vm.dump(os.path.expanduser("~/okvm/fr_home.ppm"))

# ---- page 2: TheProject via the address bar ----
n1c = len(re.findall(r"parse: count=\d+", vm.serial()))
vm.type_string("g")
time.sleep(1)
vm.type_string(P2_URL + "\n")
parsed2 = wait_parses(vm, n1c)
check("theproject fetched+parsed", parsed2)
links2 = vm.link_regions()
h2 = " ".join(r[5] for r in links2)
check("theproject key links present", all(s in h2 for s in PROJECT_MUST), "(%d links)" % len(links2))
vm.dump(os.path.expanduser("~/okvm/fr_project.ppm"))
check("no triple fault", "triple fault" not in vm.serial().lower())
vm.kill()

print("\nVERDICT:", "PASS" if not fails else "FAIL %s" % (fails,))
sys.exit(0 if not fails else 1)
