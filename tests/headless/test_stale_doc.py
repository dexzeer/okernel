#!/usr/bin/env python3
"""Headless regression: navigating to a second page must not show ghost
content from the first (stale doc_chars/doc_attrs blitted as current content).
Serves two local fixtures over HTTP (10.0.2.2 -> host loopback): a long,
text-dense page followed by a near-empty one; asserts the empty page's
content area stays essentially blank."""
import sys, os, time, re, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

FIX = os.path.expanduser("~/okvm/fixtures")
os.makedirs(FIX, exist_ok=True)
with open(os.path.join(FIX, "a.html"), "w") as f:
    f.write("<html><head><style>body{background:#000;color:#fff}</style>"
            "</head><body><h1>LONGDENSEPAGE</h1>")
    for i in range(40):
        f.write(f"<p>Paragraph {i} " + ("lorem ipsum dolor sit amet " * 12) + "</p>")
    f.write("</body></html>")
with open(os.path.join(FIX, "b.html"), "w") as f:
    f.write('<html><body style="background:#000;color:#fff">')
    # Several SHORT lines: each leaves most of its row unwritten, so any
    # glyph past the word is a stale cell from page A's full-width text.
    for w in ["hi", "one", "two", "three", "four", "five", "six",
              "seven", "eight", "nine", "ten", "eleven", "twelve"]:
        f.write(f"<p>{w}</p>")
    f.write("</body></html>")

subprocess.Popen([sys.executable, "-m", "http.server", "8137", "--bind",
                  "127.0.0.1", "--directory", FIX],
                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)

def parses(log):
    return len(re.findall(r"\[br\] (?:https )?parse: count=\d+", log))

results = []
try:
    vm = OkVM("stale")
    time.sleep(14)

    # Page A: dense, fills the doc grid
    vm.type_string("okai http://10.0.2.2:8137/a.html\n")
    results.append(("page A loads", vm.wait_for("parse: count=", timeout=70)))
    time.sleep(3)
    vm.dump()

    # Page B: nearly empty — any other visible text is a stale-grid ghost
    vm.type_string("g"); time.sleep(0.5)
    vm.type_string("10.0.2.2:8137/b.html\n")
    results.append(("page B loads",
                    vm.wait_for("parse: count=", timeout=70) and parses(vm.serial()) >= 2))
    time.sleep(3)
    vm.dump(os.path.expanduser("~/okvm/stale_b.ppm"))

    from PIL import Image
    img = Image.open(os.path.expanduser("~/okvm/stale_b.ppm")).convert("RGB")
    px = img.load()
    # Content area of the okai window (window at x=1010,y=60, chrome ~96px tall)
    lit = 0
    for y in range(60 + 100, 60 + 640):
        for x in range(1015, 1880):
            r, g, bl = px[x, y]
            if r > 180 and g > 180 and bl > 180:
                lit += 1
    results.append((f"page B content near-blank (lit={lit})", lit < 6000))
    img.save(os.path.expanduser("~/okvm/stale_b.png"))

    vm.kill()
except Exception as e:
    print("ERROR:", e)
    results.append(("no crash", False))
    try: vm.kill()
    except Exception: pass

all_ok = True
for name, ok in results:
    print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    all_ok &= ok
print("\nVERDICT:", "PASS" if all_ok else "FAIL")
sys.exit(0 if all_ok else 1)
