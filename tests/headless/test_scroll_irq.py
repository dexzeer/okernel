#!/usr/bin/env python3
"""Scroll + mouse IRQ stress (offline: a tall fixture page served from the
host on :18091). Wheel-scrolls okai down and back up (scrolling up is a
backward memmove of the page surface: `std` + rep movs) while the mouse
wiggles, so mouse IRQs — which composite the cursor and copy memory — land
inside those copies (the timer IRQ lands there every 10 ms). Before the ISR
stubs cleared DF, a handler interrupting the backward copy ran its own
copies backwards and corrupted the kernel (hang with DF=1, IF=0); the
irq_handler canary logs `[isr] DF set` if that cld ever goes missing.

    python3 tests/headless/test_scroll_irq.py
"""
import sys, os, time, re, subprocess, threading, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

WWW = tempfile.mkdtemp(prefix="okscroll_")
with open(os.path.join(WWW, "long.html"), "w") as f:
    f.write("<html><head><title>long</title></head><body>\n")
    for i in range(400):
        f.write(f"<p>Paragraph {i}: the quick brown fox jumps over the lazy dog. "
                f"KAnarchy scroll stress line {i}.</p>\n")
    f.write("</body></html>\n")
srv = subprocess.Popen([sys.executable, "-m", "http.server", "18091", "--directory", WWW],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

results = []
def check(name, ok, info=""):
    results.append(bool(ok))
    print(f"    {'PASS' if ok else 'FAIL'}  {name}  {info}", flush=True)

vm = OkVM("scrollirq")
try:
    vm.wait_for("[mem] heap", timeout=60); time.sleep(6)
    vm.type_string("okai http://10.0.2.2:18091/long.html\n")
    check("page loaded", vm.wait_for("page origin", timeout=90))
    time.sleep(4)
    stop = [False]
    def wiggle():
        d = 1
        while not stop[0]:
            vm.mon(f"mouse_move {5 * d} {3 * d}", 0.01)
            d = -d
    th = threading.Thread(target=wiggle); th.start()
    mark = len(vm.serial())
    for _ in range(3):
        for _ in range(12): vm.mon("mouse_move 0 0 -1", 0.08)   # down
        for _ in range(12): vm.mon("mouse_move 0 0 1", 0.08)    # up (backward memmove)
    stop[0] = True; th.join()
    time.sleep(4)
    seg = vm.serial()[mark:]
    wheels = len(re.findall(r"\[okai\] wheel", seg))
    scrolls = len(re.findall(r"\[okai\] scroll dy=", seg))
    ups = len(re.findall(r"\[okai\] scroll dy=-", seg))
    check("every wheel event handled", wheels == 72, f"wheel={wheels}")
    check("scrolled both ways", scrolls >= 60 and ups >= 30, f"scrolls={scrolls} up={ups}")
    n0 = len(re.findall(r"\[fps\]", vm.serial())); time.sleep(3)
    n1 = len(re.findall(r"\[fps\]", vm.serial()))
    check("kernel alive afterwards", n1 - n0 >= 2, f"fps lines in 3s: {n1 - n0}")
finally:
    vm.kill()
    srv.terminate()

faults = [l for l in vm.serial().splitlines()
          if "PAGE FAULT" in l.upper() or "[ISR] Exception" in l or "kmalloc FAIL" in l]
check("IRQ handlers always see DF=0 (stub cld)", "[isr] DF set" not in vm.serial())
check("no faults", not faults, str(faults[:3]))
ok = all(results)
print("SCROLL_IRQ", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
