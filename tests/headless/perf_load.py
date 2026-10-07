#!/usr/bin/env python3
"""Page-load profiler: loads each URL through the address bar and prints a
timeline of the fetch / parse / render / script events, stamped with host
time as the serial lines arrive (50ms polling — the kernel logs no clock).

    python3 tests/headless/perf_load.py [tag] [url ...]

Per page it reports: time to document (navigate -> parse), time to first
render, and time until the page settles (no fetch/render/js line for 6s),
plus per-resource fetch times. Needs internet.
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

DEFAULT = ["https://en.wikipedia.org/wiki/Operating_system", "https://www.python.org/",
           "https://www.bbc.com/", "https://example.com/"]

KEY = re.compile(r"\[okai\] (navigate|fetch|sub-res|render|js |error page)|"
                 r"\[tls-net\] (queued|DNS resolved|TCP established|received|handshake/download|fetch timed|HTTP message complete)|"
                 r"\[tls\] (certificate chain verified|OCSP staple verdict)|"
                 r"parse: count=|\[http\] (sending|connection closed)|\[wjs\]|\[js\]")
# "settled" = no network activity (live pages keep re-rendering forever)
ACTIVITY = re.compile(r"\[okai\] (fetch|sub-res)|\[tls-net\]|\[http\]|parse: count=")

args = sys.argv[1:]
tag = args.pop(0) if args and not args[0].startswith("http") else "perf"
urls = args or DEFAULT
PERF_ALL = os.environ.get("PERF_ALL") == "1"   # stamp every line (minus per-packet noise)
NOISE = re.compile(r"e1000_(irq|rx|tx)|\[poll\]|\[fps\]|link\[|field\[")
SETTLE = float(os.environ.get("PERF_SETTLE", "6"))
verbose = os.environ.get("PERF_VERBOSE", "1") == "1"

vm = OkVM(tag)
summary = []
try:
    vm.wait_for("[mem] heap", timeout=40)
    time.sleep(10)
    for cmd in filter(None, os.environ.get("PERF_PRE", "").split(";")):   # e.g. PERF_PRE=nettrace
        vm.type_string(cmd + "\n")
        time.sleep(1)
    vm.type_string("okai\n")
    vm.wait_for("home rendered", timeout=40)
    time.sleep(3)
    for url in urls:
        pos = len(vm.serial())
        vm.type_string("\x1b")   # Esc: leave any field a page script focused
        time.sleep(0.2)
        vm.type_string("g")
        time.sleep(0.4)
        vm.type_string(url + "\n")
        t0 = None
        last_act = time.time()
        events = []
        buf = ""
        start = time.time()
        while time.time() - start < 240:
            s = vm.serial()
            new, pos = s[pos:], len(s)
            now = time.time()
            buf += new
            lines = buf.split("\n")
            buf = lines.pop()
            for ln in lines:
                if t0 is None and "[okai] navigate" in ln:
                    t0 = now
                if t0 is None:
                    continue
                if KEY.search(ln) or (PERF_ALL and not NOISE.search(ln)):
                    events.append((now - t0, ln.strip()))
                if ACTIVITY.search(ln):
                    last_act = now
            if t0 is not None and now - last_act > SETTLE:
                break
            time.sleep(0.05)
        def first(pat):
            for t, l in events:
                if re.search(pat, l): return t
            return None
        doc = first(r"parse: count=")
        rend = first(r"\[okai\] render tab")
        settle = (last_act - t0) if t0 else None
        nsub = sum(1 for _, l in events if "[okai] sub-res fetch" in l)
        nfetch = sum(1 for _, l in events if "[tls-net] queued" in l or "[http] sending" in l)
        js = sum(int(m.group(1)) for _, l in events for m in [re.search(r"js tab=\d+ ran (\d+)ms", l)] if m)
        rms = sum(int(m.group(1)) for _, l in events for m in [re.search(r"render tab=.* in (\d+)ms", l)] if m)
        print("\n=== %s" % url)
        if verbose:
            for t, l in events:
                print("  %7.2fs  %s" % (t, l[:150]))
        fmt = lambda v: "%.1fs" % v if v is not None else "-"
        line = ("SUMMARY %-48s doc=%s first_render=%s settled=%s fetches=%d subres=%d js_ms=%d render_ms=%d"
                % (url[:48], fmt(doc), fmt(rend), fmt(settle), nfetch, nsub, js, rms))
        print(line, flush=True)
        summary.append(line)
finally:
    vm.kill()
print()
for l in summary: print(l)
