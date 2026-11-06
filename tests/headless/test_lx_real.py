#!/usr/bin/env python3
"""Real programs of the host on the subsystems (see tools/lx/real_progs.sh, which builds the image inputs).
A program the host does not have is skipped, not failed. Screens go to ~/okvm/ for a look.
"""
import os, re, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("OKVM_ISO", os.path.join(HERE, "..", "..", "build-host", "lxtest.iso"))
import okvm
from okvm import OkVM

ROOT = os.path.join(HERE, "..", "..", "build-host", "extra_real")
fails = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails.append(what)

# (program file under the image root, command line, what its output must contain)
CASES = [
    ("bin/grep", "lx /bin/grep -n ap /data/words.txt", "4:apricot jam"),
    ("bin/sed", "lx /bin/sed -n 2p /data/words.txt", "banana split"),
    ("bin/awk", "lx /bin/awk 'NR==3{print $2}' /data/words.txt", "tart"),
    ("bin/find", "lx /bin/find /data", "/data/words.txt"),
    ("bin/tar", "lx /bin/tar cf /tmp/a.tar /data", None),
    ("bin/tar", "lx /bin/tar tf /tmp/a.tar", "data/words.txt"),
    ("bin/curl", "lx /bin/curl --version", "libcurl"),
    ("bin/git", "lx /bin/git init -q /tmp/r", None),
    ("bin/git", "lx /bin/git -C /tmp/r status", "No commits yet"),
    ("bin/python3", "lx /bin/python3 -c 'print(6*7)'", "42"),
    ("win/where.exe", "lx /win/where.exe /?", "WHERE"),
]

vm = OkVM("lxreal")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "desktop up")
    time.sleep(8)
    for prog, cmd, want in CASES:
        if not os.path.exists(os.path.join(ROOT, prog)):
            print("skip " + cmd)
            continue
        start = len(vm.serial())
        n = vm.serial().count("exited, status")
        vm.type_string(cmd + "\n")
        end = time.time() + 40
        while time.time() < end and vm.serial().count("exited, status") <= n: time.sleep(0.5)
        seg = vm.serial()[start:]
        m = re.search(r"exited, status (\d+)", seg)
        check(m is not None and m.group(1) == "0" and (want is None or want in seg), f"`{cmd}` -> {want!r}")
        if "unimplemented" in seg:
            print("     note:", sorted(set(re.findall(r"(?:unimplemented syscall \d+|unimplemented function \S+)", seg))))
    if os.path.exists(os.path.join(ROOT, "win/winver.exe")):
        start = len(vm.serial()); n = vm.serial().count("exited, status")
        vm.type_string("lx /win/winver.exe\n")
        time.sleep(10)
        check(re.search(r"\[winfx\] (open|restore)", vm.serial()[start:]) is not None, "winver shows its window")
        vm.mon("sendkey ret", 1)
        end = time.time() + 20
        while time.time() < end and vm.serial().count("exited, status") <= n: time.sleep(0.5)
        m = re.search(r"exited, status (\d+)", vm.serial()[start:])
        check(m is not None and m.group(1) == "0", "and ends when it is dismissed")
    if os.path.exists(os.path.join(ROOT, "win/notepad.exe")):
        def shot(name):
            p = os.path.join(okvm.OUTDIR, name + ".ppm")
            vm.mon(f"screendump {p}", 1.5)
            d = open(p, "rb").read()
            hdr = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", d)
            return int(hdr.group(1)), int(hdr.group(2)), d[hdr.end():]
        vm.type_string("lx /win/notepad.exe\n")
        time.sleep(15)
        w, h, opened = shot("np1")
        run = best = bottom = 0                                           # the text area: the tallest white column
        for y in range(h):
            run = run + 1 if opened[(y * w + 300) * 3:(y * w + 300) * 3 + 3] == b"\xff\xff\xff" else 0
            if run > best: best, bottom = run, y
        check(best > 100, "notepad opens")
        if best > 100:
            vm.type_string("ab\ncd"); time.sleep(4)
            w, h, typed = shot("np2")
            # the status bar is the strip under the text area
            status = sum(1 for y in range(bottom + 1, bottom + 24) for x in range(0, w) if opened[(y * w + x) * 3:(y * w + x) * 3 + 3] != typed[(y * w + x) * 3:(y * w + x) * 3 + 3])
            check(status > 0, f"its status bar follows the caret ({status} pixels changed)")
    log = vm.serial()
    check(not re.search(r"\[(ISR|panic)\]", log), "no kernel faults")
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
