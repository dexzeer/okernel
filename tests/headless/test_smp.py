#!/usr/bin/env python3
"""SMP bring-up and the job system: boot with N CPUs, check every AP came up,
run `smp test` (parallel results, publish/recycle protocol, heap contention,
nested calls) and make sure the desktop is still alive afterwards.

    python3 tests/headless/test_smp.py [cpus ...]        default: 1 2 4 8
x2APIC and real-virtualization runs: tests/headless/smp_win.ps1 (TCG cannot do x2APIC)
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import okvm
from okvm import OkVM

configs = []                           # (label, cpus, x2apic)
for a in sys.argv[1:] or ["1", "2", "4", "8"]:
    x2 = a.startswith("x2:")
    configs.append((a, int(a.split(":")[-1]), x2))
all_ok = True

DEFAULT_ISO = okvm.ISO
for label, n, x2 in configs:
    os.environ["OKVM_SMP"] = str(n)
    if x2:
        os.environ["OKVM_CPU"] = "max"
        okvm.ISO = os.path.join(okvm.PROJECT, "kanarchy-x2apic.iso")
    else:
        os.environ.pop("OKVM_CPU", None)
        okvm.ISO = DEFAULT_ISO
    vm = OkVM("smp" + label.replace(":", "_"))
    ok = True
    try:
        if not vm.wait_for("[mem] heap", timeout=60):
            print(f"  -smp {n}: FAIL no heap line"); all_ok = False; vm.kill(); continue
        time.sleep(8)
        log = vm.serial()
        m = re.search(r"\[smp\] (\d+) CPUs running", log)
        running = int(m.group(1)) if m else 1
        acpi = re.search(r"\[acpi\] RSDP \w+ rev \d+: (\d+) cpus", log)
        print(f"  -smp {n}: acpi cpus={acpi.group(1) if acpi else '?'}  running={running}", flush=True)
        mode = "x2APIC" if "(x2APIC)" in log else "xAPIC"
        print(f"    boot cpu mode: {mode}", flush=True)
        if x2:
            ok = ok and mode == "x2APIC"
        if n > 1:
            ok = ok and acpi and int(acpi.group(1)) == n and running == n
        else:
            ok = ok and running == 1
        # Every CPU must stay quiet: no fault lines.
        if "[smp] cpu" in log and "fault" in log:
            print("    AP FAULT:", re.findall(r"\[smp\] cpu .*fault.*", log)[:3]); ok = False
        vm.type_string("smp\n", delay=0.03)
        time.sleep(1)
        vm.type_string("smp test\n", delay=0.03)
        done = vm.wait_for(r"smp selftest: (ALL OK|FAILED)", timeout=120)
        log = vm.serial()
        res = re.findall(r"\[smp\] ([ABCDE] [^\n]*)", log)
        for r in res:
            print("    " + r.strip())
        passed = "smp selftest: ALL OK" in log
        print(f"    selftest: {'PASS' if passed else 'FAIL'}", flush=True)
        ok = ok and passed
        # The shell still answers after the APs worked.
        mark = len(vm.serial())
        vm.type_string("echo alive-after-smp\n", delay=0.03)
        time.sleep(2)
        alive = "[sh] exec: echo" in vm.serial()[mark:]
        print(f"    shell alive: {'PASS' if alive else 'FAIL'}", flush=True)
        ok = ok and alive
        if "[ISR]" in vm.serial():
            print("    kernel exception:", re.findall(r"\[ISR\][^\n]*", vm.serial())[:2]); ok = False
    finally:
        vm.kill()
    print(f"  -smp {n}: {'PASS' if ok else 'FAIL'}", flush=True)
    all_ok = all_ok and ok

sys.exit(0 if all_ok else 1)
