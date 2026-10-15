#!/usr/bin/env python3
"""YouTube's home page loads in okai (online test, like test_sites.py).

    python3 tests/headless/test_okai_yt_home.py [tag]

What each assertion guards (all three used to leave a blank page):
  - the hidden sign-in <iframe> is loaded ONCE (its src used to be compared with the redirected URL and reloaded forever),
  - every request gets out: the pool no longer fills up with idle HTTP/2 sessions (fonts.gstatic.com icons were starved),
  - the app shell rendered: the page grew past the 936 px blank-page height only if the feed card stamped, and the icon
    SVGs (XHR to gstatic) were answered.
PASS needs network access to youtube.com AND a fast guest (OKVM_ACCEL=kvm): under TCG the page's scripts need minutes.
On Windows use tests\headless\yt_home_win.ps1 (WHPX) - same assertions on the serial log.
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

tag = sys.argv[1] if len(sys.argv) > 1 else "yt_home"
if not os.environ.get("OKVM_ACCEL"):
    print("RESULT SKIP (needs OKVM_ACCEL=kvm; TCG is too slow for YouTube)")
    sys.exit(0)
vm = OkVM(tag)
ok = False
try:
    vm.wait_for("[mem] heap", timeout=40)
    time.sleep(10)
    vm.type_string("okai https://www.youtube.com/\n")
    vm.wait_for("parse: count=", timeout=120, fail_pats=["[okai] error page:"])
    t0 = time.time()                                 # scripts (seconds on a slow guest), then the icons the masthead asks for
    while time.time() - t0 < 180:
        log = vm.serial()
        asked = len(re.findall(r"queued HTTPS fonts\.gstatic\.com/s/i/", log))
        if asked and len(re.findall(r"received \d+ bytes \(fonts\.gstatic\.com/s/i/", log)) >= asked:
            break
        time.sleep(3)
    time.sleep(5)
    log = vm.serial()
    reloads = len(re.findall(r"\[okai\] frame \d+: src ->", log))
    icons = len(re.findall(r"queued HTTPS fonts\.gstatic\.com/s/i/", log))
    got = len(re.findall(r"received \d+ bytes \(fonts\.gstatic\.com/s/i/", log))
    print("iframe reloads:", reloads, " icon requests:", icons, " icon answers:", got)
    fails = []
    if reloads > 2: fails.append("sign-in iframe reloaded %d times" % reloads)
    if icons == 0 or got < icons: fails.append("icons: %d asked, %d answered" % (icons, got))
    if "PAGE FAULT" in log.upper() or "kmalloc FAIL" in log: fails.append("fault in log")
    ok = not fails
    print("RESULT", "PASS" if ok else "FAIL " + "; ".join(fails))
finally:
    vm.kill()
sys.exit(0 if ok else 1)
