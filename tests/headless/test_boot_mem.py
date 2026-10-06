#!/usr/bin/env python3
"""Boot smoke test for the kernel heap (Phase 0): boots, runs `mem`, opens
the browser home page, and checks the serial log for heap init and the
absence of faults / allocator diagnostics. OKVM_MEM selects the RAM size."""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

tag = sys.argv[1] if len(sys.argv) > 1 else "bootmem"
vm = OkVM(tag)
try:
    ok = vm.wait_for("[mem] heap", timeout=40)
    time.sleep(10)
    vm.type_string("mem\n")
    time.sleep(2)
    vm.type_string("okai\n")
    home = vm.wait_for(r"home rendered", timeout=40)
    time.sleep(3)
    vm.dump()
    out = os.environ.get("OKVM_PNG")
    if out:  # convert the PPM screendump for viewing
        from PIL import Image
        Image.open(vm.PPM).save(out)
    log = vm.serial()
    bad = [l for l in log.splitlines()
           if "kmalloc FAIL" in l or "bad or double free" in l or
              "EXCEPTION" in l.upper() and "#" in l or "PAGE FAULT" in l.upper()]
    heap = [l for l in log.splitlines() if "[mem] heap" in l or "high alias" in l]
    print("\n".join(heap))
    print("home rendered:", bool(home))
    print("bad lines:", bad[:10])
    print("RESULT:", "PASS" if ok and home and not bad else "FAIL")
finally:
    vm.kill()
