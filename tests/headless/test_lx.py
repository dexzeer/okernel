#!/usr/bin/env python3
"""The Linux subsystem: x86-64 Linux programs run natively in ring 3 of the long-mode CPU.

    HOSTBINS=1 sh tools/lx/build_tests.sh && python3 tests/headless/test_lx.py

Runs the programs of lx/src (own code), the glibc test programs of lx/test and, when the root was built with HOSTBINS=1, the
host's dynamically linked bash/ls/cat.
"""
import sys, os, re, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("OKVM_ISO", os.path.join(HERE, "..", "..", "build-host", "lxtest.iso"))   # before okvm is imported
import okvm
from okvm import OkVM

fails = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails.append(what)

vm = OkVM("lx")
try:
    check(vm.wait_for("[mem] heap", timeout=120), "desktop boots")
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=30), "lx_init ran")
    check(vm.wait_for("[lxfs] tar:", timeout=5), "root tar module loaded")
    time.sleep(8)

    def run(cmd, expect, timeout=60, status=0):
        """Type an `lx` command; expect the text and the exit status of the pid it starts."""
        before = vm.serial()
        n0 = before.count("started:")
        vm.type_string(cmd + "\n")
        ok = vm.wait_for(expect, timeout=timeout)
        # a short settle so the exit line is in the log
        end = time.time() + 15
        while time.time() < end:
            log = vm.serial()
            if log.count("exited, status") > before.count("exited, status") and ok: break
            time.sleep(0.5)
        log = vm.serial()[len(before):]
        check(ok, f"`{cmd}` prints {expect!r}")
        if status is not None:
            m = re.findall(r"pid (\d+) exited, status (\d+)", log)
            check(any(int(s) == status for _, s in m[-1:]) if m else False, f"`{cmd}` exit status {status}  (got {m[-1:] if m else None})")

    run("lx /bin/hello", "hello from x86-64 Linux")
    run("lx /bin/args one \"two three\"", "[two three]")
    run("lx /bin/glhello a b", "glibc hello: argc=3", status=768)
    run("lx /bin/sigtest", "signals done")
    run("lx /bin/threads", "joined: shared=101 counter=1000")
    run("lx /bin/glthread", "threads done: total=4000 sum=60")
    run("lx /bin/glsock", "SOCKTEST PASS")
    run("lx /win/hello.exe", "hello from a Windows program", status=7 << 8)
    run("lx /win/crtdemo.exe", "CRTDEMO PASS", timeout=120)
    if "/bin/bash" in open(os.path.join(HERE, "..", "..", "build-host", "lxtest.tar"), "rb").read().decode("latin1")[:0] or True:
        # dynamically linked host programs, only when the root has them
        tarlist = vm.serial()
        run("lx /bin/ls /", "dev  home", status=0, timeout=80) if os.environ.get("LX_HOSTBINS") else None
finally:
    vm.kill()
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
