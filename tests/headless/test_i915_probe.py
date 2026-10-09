#!/usr/bin/env python3
"""The Intel-graphics probe and `gpu` report, exercised without an Intel GPU: the
test kernel (`make kanarchy-i915test.iso`) matches QEMU's standard VGA (vendor
0x1234) and pretends to be Gen9, so PCI identification, BAR sizing, the uncached
MMIO mapping and the register/GTT report all run for real. Checks the boot stays
healthy, the probe logs the device, `gpu` answers with the expected lines, and the
display (still the GRUB framebuffer) is untouched.

    python3 tests/headless/test_i915_probe.py
"""
import sys, os, re, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import okvm
from okvm import OkVM

subprocess.run(["make", "kanarchy-i915test.iso"], cwd=okvm.PROJECT, check=True, stdout=subprocess.DEVNULL)
okvm.ISO = os.path.join(okvm.PROJECT, "kanarchy-i915test.iso")
vm = OkVM("i915probe")
ok = True
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    log = vm.serial()
    probe = re.search(r"\[i915\] Intel graphics ([0-9a-f]+):([0-9a-f]+) rev ([0-9a-f]+) at (\d+):(\d+)\.(\d+): (.*), BAR0 ([0-9a-f]+) \((\d+) KB\)", log)
    print("  probe line:", probe.group(0)[:110] if probe else None)
    ok = ok and probe is not None
    if probe:
        ok = ok and int(probe.group(9)) == 16384        # std VGA BAR0 = 16 MB framebuffer
    ok = ok and "[gfx] display backend: GRUB framebuffer" in log
    vm.type_string("gpu\n", delay=0.03)
    time.sleep(3)
    log = vm.serial()
    seg = log[log.rindex("[sh] exec: gpu"):]
    lines = re.findall(r"\[smp\] ([^\n]*)", seg)
    for l in lines[:12]:
        print("   ", l)
    need = [r"Intel .*rev", r"BAR0 \(regs\+GTT\)", r"GRUB framebuffer at", r"pipe A"]
    for n in need:
        if not any(re.search(n, l) for l in lines):
            print(f"    FAIL  report lacks /{n}/"); ok = False
    if re.search(r"\[ISR\]|PAGE FAULT", log):
        print("    FAIL  kernel exception"); ok = False
    # the desktop still works
    vm.type_string("echo still-alive\n", delay=0.03); time.sleep(2)
    if "[sh] exec: echo" not in vm.serial()[len(log) - 50:]:
        print("    FAIL  shell dead after gpu report"); ok = False
finally:
    vm.kill()
print("I915_PROBE", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
