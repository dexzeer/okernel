#!/usr/bin/env python3
"""Headless test: external CSS/JS fetching + JS DOM bridge."""
import sys, os, time, subprocess, signal, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

SERVER_DIR = "/tmp/www"
server = subprocess.Popen(
    ["python3", "-m", "http.server", "8000"],
    cwd=SERVER_DIR,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
time.sleep(1)

vm = OkVM("subres")
time.sleep(14)

results = []

# TEST 1: Navigate to test page with external CSS + JS
vm.type_string("okai http://10.0.2.2:8000/test_css_js.html\n")
parsed = vm.wait_for("parse: count=", timeout=70)
results.append(("page loads", parsed))

time.sleep(5)
log = vm.serial()

# TEST 2: External CSS was fetched
has_css = "[okai] sub-res fetch: CSS" in log
results.append(("external CSS fetched", has_css))

# TEST 3: External JS was executed
has_js = "sub-res JS: executed" in log or "[js] External JS executed!" in log
results.append(("external JS executed", has_js))

# TEST 4: JS DOM bridge setText worked
has_settext = "[js] setText(" in log
results.append(("JS setText called", has_settext))

# TEST 5: JS DOM bridge setStyle worked
has_setstyle = "[js] setStyle(" in log
results.append(("JS setStyle called", has_setstyle))

# TEST 6: Sub-resource queue was populated
has_queue = "sub-res:" in log and "queued" in log
results.append(("sub-res queue populated", has_queue))

vm.dump()
vm.kill()
server.terminate()
server.wait(timeout=5)

print("\n--- Sub-resource + DOM bridge test results ---")
all_ok = True
for name, ok in results:
    print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    all_ok &= ok

print("\n--- Relevant serial lines ---")
for line in log.split('\n'):
    if any(tag in line for tag in ['[js]', '[br]', 'sub-res', 'parse:', 'css rules']):
        print(f"  {line}")

print("\nVERDICT:", "PASS" if all_ok else "FAIL")
sys.exit(0 if all_ok else 1)
