#!/usr/bin/env python3
"""Search from the terminal and the address bar: `search WORDS`, `search engine NAME`, and text typed into okai's
address bar that is not an address. Offline: only the navigation URL in the serial log is checked."""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

tag = sys.argv[1] if len(sys.argv) > 1 else "search"
vm = OkVM(tag)
ok = True


def step(name, cond, extra=""):
    global ok
    print(("PASS " if cond else "FAIL ") + name + (" " + extra if extra else ""))
    ok = ok and bool(cond)


def navs():
    return [l.split("navigate ", 1)[1].strip() for l in vm.serial().splitlines() if "[okai] navigate " in l]


DDG = "https://html.duckduckgo.com/html/?q="
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("search kernel panic\n", delay=0.05)
    step("search WORDS goes to the default engine", vm.wait_for("navigate " + DDG + "kernel+panic", timeout=30), str(navs()[-1:]))
    time.sleep(2)
    # the browser window has the focus now: `g` opens its address bar
    vm.type_string("g", delay=0.05)
    vm.type_string("hello world\n", delay=0.05)
    step("address bar words are searched", vm.wait_for("navigate " + DDG + "hello+world", timeout=30), str(navs()[-1:]))
    time.sleep(2)
    vm.type_string("g", delay=0.05)
    vm.type_string("example.com\n", delay=0.05)
    step("a host name is still a host name", vm.wait_for("navigate https://example.com", timeout=30), str(navs()[-1:]))
    time.sleep(2)
    vm.type_string("g", delay=0.05)
    vm.type_string("kernel\n", delay=0.05)
    time.sleep(3)
    step("a single word is searched", DDG + "kernel" in navs(), str(navs()[-1:]))
    bad = [l for l in vm.serial().splitlines() if "PAGE FAULT" in l or "[ISR] Exception" in l]
    step("no faults", not bad, str(bad[:2]))
finally:
    vm.kill()
print("SEARCH " + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
