#!/usr/bin/env python3
"""Load one page in okai and screenshot it (web engine end-to-end).

    python3 tests/headless/test_okai_page.py <url> [tag] [--scroll N]

Boots the desktop, runs `okai <url>`, waits for the document parse and the
sub-resource drain (stylesheets + images), saves ~/okvm/<tag>.png, and
optionally presses space N times (page down) and saves <tag>_scroll.png.
PASS = the page parsed and rendered without faults / allocator errors.
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR

url = sys.argv[1] if len(sys.argv) > 1 else "http://example.com/"
tag = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else "page"
scroll = 0
if "--scroll" in sys.argv:
    scroll = int(sys.argv[sys.argv.index("--scroll") + 1])

def png(vm, name):
    vm.dump()
    from PIL import Image
    out = os.path.join(OUTDIR, name + ".png")
    Image.open(vm.PPM).save(out)
    return out

vm = OkVM(tag)
try:
    vm.wait_for("[mem] heap", timeout=40)
    time.sleep(10)
    vm.type_string("okai " + url + "\n")
    # a document parse, or an error page (unreachable host, cert failure...)
    parsed = vm.wait_for("parse: count=", timeout=120, fail_pats=["[okai] error page:"])
    errpage = "[okai] error page:" in vm.serial()
    # sub-resources: wait for the drain (or give up after a while)
    t0 = time.time()
    while time.time() - t0 < 150:
        log = vm.serial()
        if "sub-res done" in log or errpage or ("parse: count=" in log and "sub-res fetch" not in log and time.time() - t0 > 8):
            break
        time.sleep(2)
    time.sleep(4)
    shot = png(vm, tag)
    if scroll:
        for _ in range(scroll):
            vm.type_string(" ")
            time.sleep(0.8)
        time.sleep(3)
        png(vm, tag + "_scroll")
    log = vm.serial()
    bad = [l for l in log.splitlines()
           if "kmalloc FAIL" in l or "bad or double free" in l or
              ("EXCEPTION" in l.upper() and "#" in l) or "PAGE FAULT" in l.upper()]
    for l in log.splitlines():
        if re.search(r"\[br\]|\[okai\] (render|sub-res done|redirect|fetch|https failed|TLS)|gzip", l):
            print(l[:200])
    print("screenshot:", shot)
    print("bad lines:", bad[:10])
    if errpage:
        print([l for l in log.splitlines() if "error page:" in l][-1])
    print("RESULT:", "PASS" if (parsed or errpage) and not bad else "FAIL")
finally:
    vm.kill()
