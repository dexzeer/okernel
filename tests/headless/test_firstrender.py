#!/usr/bin/env python3
"""Headless render regression: the first website must parse, lay out, and be
navigable in the real guest (covers the HTML inline/nesting rewrite: linked
<li> bullets, uppercase </A>, DT/DD, whitespace collapse, wrapped-link
spans). Page 2 goes through the address bar (keyboard = geometry-free);
link hit-testing stays covered by test_link_click.py.

Verdict from serial ground truth + screenshots for vision.
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

def link_spans(vm):
    """(i, row, col0, col1, endrow, href) parsed from serial link lines."""
    out = []
    for line in vm.serial().splitlines():
        m = re.search(
            r"\[okai\] link\[(\d+)\] row=(\d+) col0=(\d+) col1=(\d+)"
            r" href=(\S+)(?: endrow=(\d+))?", line)
        if m:
            endrow = int(m.group(6)) if m.group(6) is not None else int(m.group(2))
            out.append((int(m.group(1)), int(m.group(2)), int(m.group(3)),
                        int(m.group(4)), endrow, m.group(5)))
    return out

def wait_parses(vm, before, timeout=150):
    t0 = time.time()
    while time.time() - t0 < timeout:
        n = len(re.findall(r"parse: count=\d+", vm.serial()))
        if n > before:
            return True
        time.sleep(1)
    return False

vm = OkVM("fr")
time.sleep(14)

# ---- page 1: the home page over plain HTTP ----
vm.type_string("okai http://info.cern.ch/\n")
n0 = len(re.findall(r"parse: count=\d+", vm.serial()))
ok1 = wait_parses(vm, n0)
check("home fetched+parsed", ok1)

m = re.search(r"parse: count=(\d+)", vm.serial())
n1 = int(m.group(1)) if m else 0
check("home token count sane (>=12)", n1 >= 12, "(count=%d)" % n1)

links = link_spans(vm)
hrefs = {h for _, _, _, _, _, h in links}
check("home has 4+ link regions", len(links) >= 4, "(%d)" % len(links))
missing = {h for h in HOME_HREFS if not any(h in x or x in h for x in hrefs)}
check("home hrefs exact", not missing, "(missing=%s)" % (missing,))

# spans: single-row links need real width; wrapped links need endrow>row
# (the old bug recorded 1-char spans at wrap points)
bad = [l for l in links
       if not (l[4] > l[1] or l[3] - l[2] >= 2)]
check("no degenerate 1-char link spans", not bad, "(%s)" % (bad,))
wrapped = [l for l in links if l[4] > l[1]]
print("wrapped multi-row links: %d" % len(wrapped))

vm.dump(os.path.expanduser("~/okvm/fr_home.ppm"))
time.sleep(1)

# ---- page 2: TheProject via the address bar (no mouse geometry involved) ----
vm.type_string("g")
time.sleep(1)
vm.type_string(P2_URL + "\n")
n1c = len(re.findall(r"parse: count=\d+", vm.serial()))
parsed2 = wait_parses(vm, n1c)
check("theproject fetched+parsed", parsed2)

log = vm.serial()
m2 = re.findall(r"parse: count=(\d+)", log)
check("theproject token count sane (>=60)", parsed2 and int(m2[-1]) >= 60,
      "(counts=%s)" % (m2,))
links2 = link_spans(vm)
h2 = " ".join(h for _, _, _, _, _, h in links2)
check("theproject key links present",
      all(s in h2 for s in PROJECT_MUST), "(%d links)" % len(links2))
bad2 = [l for l in links2
        if not (l[4] > l[1] or l[3] - l[2] >= 2)]
check("theproject spans sane", not bad2, "(%s)" % (bad2[:3],))
vm.dump(os.path.expanduser("~/okvm/fr_project.ppm"))
check("no triple fault", "triple fault" not in log.lower())
vm.kill()

print("\nVERDICT:", "PASS" if not fails else "FAIL %s" % (fails,))
sys.exit(0 if not fails else 1)
