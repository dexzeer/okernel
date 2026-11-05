#!/usr/bin/env python3
"""The terminal behind Linux programs: escape sequences into the cell grid (lx/src/vtcheck.c), then real full-screen
editors driven by keys and judged by the files they save. The host programs come from tools/lx/real_progs.sh; those
without a copy there are skipped. Name resolution needs the network.
"""
import os, re, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("OKVM_ISO", os.path.join(HERE, "..", "..", "build-host", "lxtest.iso"))
from okvm import OkVM

ROOT = os.path.join(HERE, "..", "..", "build-host", "extra_real")
fails = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails.append(what)
def have(p): return os.path.exists(os.path.join(ROOT, p))

def wait_exit(vm, n, secs=40):
    end = time.time() + secs
    while time.time() < end and vm.serial().count("exited, status") <= n: time.sleep(0.3)
    return vm.serial().count("exited, status") > n

def run(vm, cmd, secs=40):
    start = len(vm.serial()); n = vm.serial().count("exited, status")
    vm.type_string(cmd + "\n")
    done = wait_exit(vm, n, secs)
    return done, vm.serial()[start:]

vm = OkVM("lxterm")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "desktop up")
    time.sleep(8)

    done, seg = run(vm, "lx /bin/vtcheck")
    rows = {int(a): b.rstrip("\r") for a, b in re.findall(r"\[term(\d+)\]([^\n]*)", seg)}
    m = re.search(r"\[termdump\] (\d+)x(\d+)", seg)
    check(done and m is not None, "vtcheck dumped the grid")
    cols = int(m.group(1)) if m else 0
    want = {0: "azc", 1: "    def", 2: "he", 3: "adef", 4: "aXYbcdef", 5: "BBB", 6: "CCC", 7: "", 8: "keep", 9: "main!", 11: "y",
            12: "red inv 256", 13: "abc !", 14: "back-|"}
    for r, s in want.items():
        check(rows.get(r, "") == s, f"row {r}: {s!r} (got {rows.get(r, '')!r})")
    check(rows.get(10, "").endswith("x") and len(rows.get(10, "")) == cols, "the last column, then a wrap")

    if have("bin/nano"):
        n = vm.serial().count("exited, status")
        vm.type_string("lx /bin/nano /tmp/n.txt\n"); time.sleep(6)
        vm.type_string("abc"); time.sleep(0.5)
        vm.mon("sendkey left", 0.3)
        vm.type_string("Z"); time.sleep(0.5)
        vm.mon("sendkey ctrl-o", 1); vm.type_string("\n"); time.sleep(1)
        vm.mon("sendkey ctrl-x", 1)
        check(wait_exit(vm, n, 15), "nano exits")
        done, seg = run(vm, "lx /bin/cat /tmp/n.txt")
        check("abZc" in seg, "nano typed, moved with an arrow key and saved")
    if have("bin/vim"):
        n = vm.serial().count("exited, status")
        vm.type_string("lx /bin/vim -u NONE /tmp/v.txt\n"); time.sleep(8)
        vm.type_string("ihello vim"); time.sleep(0.5)
        vm.mon("sendkey esc", 0.5)
        vm.mon("sendkey left", 0.3)
        vm.type_string("iX"); time.sleep(0.3)
        vm.mon("sendkey esc", 0.5)
        vm.type_string(":wq\n")
        check(wait_exit(vm, n, 20), "vim exits")
        done, seg = run(vm, "lx /bin/cat /tmp/v.txt")
        check("hello vXim" in seg, "vim typed, moved with an arrow key and saved")
        check("unimplemented syscall 128" not in vm.serial(), "vim's signal wait is implemented")
    done, seg = run(vm, "lx /bin/echo still a normal terminal")
    check("still a normal terminal" in seg, "the terminal works after the editors left")
    if have("bin/curl"):
        done, seg = run(vm, "lx /bin/curl -s -S -m 20 http://example.com/", 30)
        check("Example Domain" in seg, f"curl resolves a name and fetches a page ({seg.strip()[-90:]!r})" if "Example Domain" not in seg else "curl resolves a name and fetches a page")
    check(not re.search(r"\[(ISR|panic)\]", vm.serial()), "no kernel faults")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails)[:600])
sys.exit(1 if fails else 0)
