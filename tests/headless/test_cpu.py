#!/usr/bin/env python3
"""`cpu` shell command: the clock report and the fault-safe MSR access (a missing MSR must not crash the kernel --
cpufreq.c relies on it for the Atom P-state MSRs)."""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

vm = OkVM(sys.argv[1] if len(sys.argv) > 1 else "cpu")
ok = True


def step(name, cond, extra=""):
    global ok
    print(("PASS " if cond else "FAIL ") + name + (" " + extra if extra else ""))
    ok = ok and bool(cond)


try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    step("boot line has TSC and core clock", vm.wait_for("[cpu] family", timeout=10))
    vm.type_string("cpu msrtest\n", delay=0.05)
    step("a missing MSR faults harmlessly, a present one reads", vm.wait_for("[cpu] msrtest PASS", timeout=15))
    vm.type_string("cpu slow 7\n", delay=0.05)
    time.sleep(2)
    vm.type_string("cpu\n", delay=0.05)
    time.sleep(2)
    log = vm.serial()
    step("no exception in the log", "[ISR]" not in log and "PAGE FAULT" not in log)
finally:
    vm.kill()
print("CPU " + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
