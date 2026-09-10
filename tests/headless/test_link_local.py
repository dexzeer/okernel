#!/usr/bin/env python3
"""Offline link-click proof: local fixture pages, no internet.
Serves ~/okvm/www-links/ on :8000, navigates via numeric URL, closed-loop
clicks the link, expects page-2 parse. Exercises the exact LINK IN-PLACE
kernel path (hit-test -> navigate -> fetch -> render) without WiFi."""
import sys, os, time, re, subprocess, signal
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

GAIN = 4.8
srv = subprocess.Popen([sys.executable, "-m", "http.server", "8000",
                        "--directory", os.path.expanduser("~/okvm/www-links")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
try:
    vm = OkVM("linklocal", extra_net=True)
    time.sleep(14)

    def mse_presses():
        out = []
        for line in vm.serial().splitlines():
            m = re.search(r"\[mse\] btn=1 x=(\d+) y=(\d+)", line)
            if m: out.append((int(m.group(1)), int(m.group(2))))
        return out

    def clamp(v): return max(-60, min(60, int(v)))

    def settle(tx, ty, iters=26):
        for _ in range(iters):
            b = len(mse_presses()); vm.click(); time.sleep(0.12)
            ps = mse_presses()
            if len(ps) > b:
                cur = ps[-1]
                if abs(cur[0]-tx) <= 4 and abs(cur[1]-ty) <= 4: return True
                vm.burst(clamp((tx-cur[0])/GAIN), clamp((ty-cur[1])/GAIN))
            else:
                vm.burst(0, 10)
        return False

    def click_until(tx, ty, expect, tries=8):
        for _ in range(tries):
            before = len(re.findall(expect, vm.serial()))
            vm.click(); time.sleep(0.6)
            if len(re.findall(expect, vm.serial())) > before:
                return True
            ps = mse_presses()
            if not ps:
                vm.burst(0, 10); time.sleep(0.2); continue
            cur = ps[-1]
            dx, dy = tx - cur[0], ty - cur[1]
            if abs(dx) <= 4 and abs(dy) <= 4:
                continue
            vm.burst(clamp(dx / GAIN), clamp(dy / GAIN))
            for _ in range(5):
                vm.mon("mouse_move 0 0", 0.05)
            time.sleep(0.3)
        return False

    def n_parse():
        return vm.serial().count("parse: count=")

    vm.type_string("okai http://10.0.2.2:8000/\n")
    t0 = time.time()
    while time.time() - t0 < 60:
        if n_parse() >= 1:
            break
        time.sleep(2)
    hub = n_parse() >= 1
    print("hub parse:", "PASS" if hub else "FAIL")
    ok = hub

    m = re.search(r"\[okai\] link\[(\d+)\] row=(\d+) col0=(\d+) col1=(\d+) href=(\S+)", vm.serial())
    assert m, "hub links not rendered"
    row, c0, c1, href = int(m.group(2)), int(m.group(3)), int(m.group(4)), m.group(5)
    print("link:", row, c0, c1, href)
    tx = 1012 + ((c0+c1)//2)*12 + 6
    ty = 62 + (row+3)*24 + 12
    assert settle(1200, 660)
    before = n_parse()  # snapshot BEFORE the click: localhost fetches parse
    hit = click_until(tx, ty, r"LINK HIT.*page2\.html")  # in <1s, faster than our polls
    print("link hit page2:", "PASS" if hit else "FAIL")
    ok = ok and hit
    t0 = time.time()
    p2 = False
    while time.time() - t0 < 60:
        if n_parse() > before:
            p2 = True
            break
        time.sleep(2)
    print("page2 parse:", "PASS" if p2 else "FAIL")
    ok = ok and p2
    vm.kill()
    print("LINKLOCAL:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)
finally:
    srv.send_signal(signal.SIGTERM)
