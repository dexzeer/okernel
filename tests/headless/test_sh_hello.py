#!/usr/bin/env python3
# QEMU regression: userland hello end-to-end + post-run crash check.
# Exercises the generation-guard change (process.c): run /bin/hello must
# print, exit 0, return to the shell, and NOT #PF with eip=esp=0 after.
import sys, time
sys.path.insert(0, 'tests/headless')
from okvm import OkVM

vm = OkVM("shhello")
time.sleep(14)
ok = True

vm.type_string("run /bin/hello\n")
hello = vm.wait_for("hello from userland", timeout=60)
print("hello runs:", "PASS" if hello else "FAIL")
ok = ok and hello

back = vm.wait_for("Back from user mode.", timeout=30)
print("back to shell:", "PASS" if back else "FAIL")
ok = ok and back

# Let the timer tick a while: the post-run #PF (tick into reaped slot)
# fired AFTER the exit announce on the old kernel.
time.sleep(8)
s = vm.serial()
crash = ("Exception 14" in s.split("Back from user mode.")[-1]
         if "Back from user mode." in s else "Exception 14" in s)
print("post-run #PF:", "FAIL (crashed)" if crash else "PASS (clean)")
if crash:
    for l in s.splitlines():
        if "Exception" in l or "eip=0" in l:
            print("   ", l.strip()[-160:])
    ok = False

# Shell still alive? Two valid owners: the kernel shell ("Commands:") or
# ring-3 /bin/sh ("user sh:" help text) if userland owns the terminal.
# Either proves interactivity; sh in particular proves exec + read + write.
vm.type_string("help\n")
alive_k = vm.wait_for("Commands:", timeout=15)
alive_sh = ("user sh:" in vm.serial())
alive = alive_k or alive_sh
print("shell alive:", "PASS" if alive else "FAIL",
      "(sh)" if alive_sh else "(kernel)" if alive_k else "")
ok = ok and alive

vm.kill()
print("SH-HELLO:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
