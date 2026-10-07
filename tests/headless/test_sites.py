#!/usr/bin/env python3
"""Real-site sweep in one VM: load each URL through the address bar and
record whether the document arrived over HTTPS and parsed, or an error
page (cert / connection) was shown. Needs internet.

    python3 tests/headless/test_sites.py [tag] [url ...]

Default list = sites that failed before the 2026-10-07 TLS fixes (root
store, OCSP delegated responders, AES-128-GCM, TCP segmentation).
OK = "[tls-net] received" + a parse with no HTTP downgrade in between
(bot-protection 403 pages count: the TLS side worked). A parse that came
from the plain-HTTP fallback is FALLBACK, never OK — the first version of
this sweep counted those 14-byte fallback pages as passes.
PASS when every site is OK.
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

DEFAULT = [
    "https://www.bbc.com/", "https://www.python.org/", "https://www.reddit.com/",
    "https://www.nytimes.com/", "https://www.microsoft.com/", "https://duckduckgo.com/",
    "https://www.apple.com/", "https://www.kernel.org/", "https://www.mozilla.org/",
    "https://www.ebay.com/", "https://open.spotify.com/", "https://www.fastly.com/",
    "https://www.godaddy.com/", "https://pypi.org/", "https://crates.io/",
]

args = [a for a in sys.argv[1:]]
tag = args.pop(0) if args and not args[0].startswith("http") else "sites"
urls = args or DEFAULT

vm = OkVM(tag)
results = []
try:
    vm.wait_for("[mem] heap", timeout=40)
    time.sleep(10)
    vm.type_string("okai\n")
    vm.wait_for("home rendered", timeout=40)
    time.sleep(2)
    for url in urls:
        start = len(vm.serial())
        vm.type_string("\x1b")   # Esc: leave any field a page script focused
        time.sleep(0.2)
        vm.type_string("g")
        time.sleep(0.4)
        vm.type_string(url + "\n")
        verdict = "TIMEOUT"
        t0 = time.time()
        while time.time() - t0 < 120:
            seg = vm.serial()[start:]
            if "[okai] error page:" in seg:
                m = re.findall(r"\[okai\] error page: (.*)", seg)
                tls = re.findall(r"\[tls-net\] (handshake/download FAILED.*|fetch timed out|giving up.*)", seg)
                verdict = "ERROR " + (m[-1] if m else "") + (" [" + tls[0] + "]" if tls else "")
                break
            m = re.search(r"parse: count=\d+ len=(\d+) .*status=(\d+)", seg)
            if m:
                if "https failed, retrying http" in seg:
                    tls = re.findall(r"\[tls-net\] (handshake/download FAILED.*|fetch timed out|giving up.*)", seg)
                    verdict = "FALLBACK %s" % (tls[0] if tls else "")
                elif "[tls-net] received" in seg:
                    verdict = "OK status=%s len=%s" % (m.group(2), m.group(1))
                else:
                    verdict = "PARSED-NO-TLS status=%s" % m.group(2)
                break
            time.sleep(1)
        el = int(time.time() - t0)
        results.append((url, verdict, el))
        print("%-32s %s (%ds)" % (url, verdict, el), flush=True)
        time.sleep(3)
    log = vm.serial()
    bad = [l for l in log.splitlines() if "PAGE FAULT" in l.upper() or "[ISR] Exception" in l or "kmalloc FAIL" in l]
    print("faults:", bad[:3])
finally:
    vm.kill()
ok = sum(1 for _, v, _ in results if v.startswith("OK"))
print("RESULT: %s (%d/%d sites over HTTPS)" % ("PASS" if ok == len(results) and results else "FAIL", ok, len(results)))
sys.exit(0 if ok == len(results) else 1)
