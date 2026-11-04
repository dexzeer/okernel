#!/usr/bin/env python3
"""The kernel's own ring-3 programs (userland/, ELF64, INT 0x80): each runs on a fresh boot (the first `run` also starts init and its sh, which
then own the terminal) and ends without a fault. Prints the programs' own output lines, which fork, pipes and exec show up in."""
import sys, os, re, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

fails = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails.append(what)

for prog in ["hello", "forktest", "pipetest", "exectest", "forkexec", "dbg2", "dbg3"]:
    vm = OkVM("userland")
    try:
        check(vm.wait_for("[mem] heap", timeout=120), prog + ": desktop boots")
        time.sleep(8)
        before = len(vm.serial())
        vm.type_string("run /bin/%s\n" % prog)
        time.sleep(15)
        new = vm.serial()[before:]
        print("--- " + prog)
        for l in new.splitlines():
            if re.match(r"\[(spawn_elf|fork\]|exec\]|ISR|panic|usys|wait)", l) or re.search(r"(PASS|FAIL|ok\b|bad ELF)", l):
                print("    " + l[:110])
        check(("'/bin/%s'" % prog) in new and "bad ELF" not in new, prog + ": starts")
        check("[ISR]" not in new and "[panic]" not in new, prog + ": no fault")
        check(("[spawn_elf]" in new) and ("destroyed process" in new or "exit" in new), prog + ": ends")
    finally:
        vm.kill()
    time.sleep(1)
print("RESULT " + ("ALLPASS" if not fails else "FAIL: " + "; ".join(fails)))
sys.exit(1 if fails else 0)
