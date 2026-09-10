#!/usr/bin/env python3
# QEMU resumption proof v2: find a ticket-issuing host, then reload it.
# Uses two `okai <url>` commands (separate tabs, same host) with
# count-based waits (never stale-matches). Proves: ticket stored ->
# PSK offered -> (accepted abbreviated | clean fallback) + page parses.
import sys, time
sys.path.insert(0, 'tests/headless')
from okvm import OkVM

HOSTS = ["https://www.google.com/", "https://github.com/",
         "https://www.wikipedia.org/", "https://example.com/"]

vm = OkVM("resume")
time.sleep(14)

import re
GAIN = 4.8

def mse_presses():
    out = []
    for line in vm.serial().splitlines():
        m = re.search(r"\[mse\] btn=1 x=(\d+) y=(\d+)", line)
        if m: out.append((int(m.group(1)), int(m.group(2))))
    return out

def clamp(v): return max(-60, min(60, int(v)))

def focus_term():
    # Click the default terminal body (40,30 900x650 — clear of the okai
    # window at 1010,60): focuses the shell without typing anything.
    # WITHOUT this, keystrokes land in the browser window after the first
    # navigation (its 'g' hotkey eats address-bar focus and mangles URLs —
    # observed: github.com typed as ithub.com). Closed-loop on [mse].
    for _ in range(26):
        b = len(mse_presses()); vm.click(); time.sleep(0.12)
        ps = mse_presses()
        if len(ps) > b:
            cur = ps[-1]
            if abs(cur[0]-200) <= 6 and abs(cur[1]-400) <= 6: return True
            vm.burst(clamp((200-cur[0])/GAIN), clamp((400-cur[1])/GAIN))
        else:
            vm.burst(0, 10)
    return False

def n_parse():
    return vm.serial().count("https parse: count=")

def fetch(url, timeout=120):
    focus_term()
    before = n_parse()
    vm.type_string("okai %s\n" % url)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if n_parse() > before:
            return True
        time.sleep(2)
    return False

ok = True
ticket_host = None
for h in HOSTS:
    if fetch(h):
        s = vm.serial()
        print(h, "parse PASS")
        if "ticket stored" in s:
            ticket_host = h
            print(h, "ISSUES TICKETS")
            break
        else:
            print(h, "no tickets")
    else:
        print(h, "fetch FAIL")

if ticket_host:
    s0 = vm.serial().count("offering PSK")
    if fetch(ticket_host):
        s = vm.serial()
        offered = vm.serial().count("offering PSK") > s0
        accepted = "resumption accepted" in s
        print("PSK offered on refetch:", "PASS" if offered else "FAIL")
        print("resumption:", "ACCEPTED (abbreviated)" if accepted else "fallback/full")
        ok = ok and offered
    else:
        print("refetch FAIL")
        ok = False
else:
    print("NO TICKET ISSUER FOUND")
    ok = False

vm.kill()
print("RESUME:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
