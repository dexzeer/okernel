#!/usr/bin/env python3
"""The DHCP client against QEMU's user-mode router (it hands out 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3)."""
import sys, time
sys.path.insert(0, 'tests/headless')
from okvm import OkVM

vm = OkVM("dhcp")
ok = False
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("dhcp\n")
    got = vm.wait_for("dhcp: address 10.0.2.15 mask 255.255.255.0 gateway 10.0.2.2 dns 10.0.2.3", timeout=30)
    time.sleep(1)
    vm.type_string("ipconfig\n"); time.sleep(2)
    log = open(vm.LOG, errors="replace").read()
    ok = bool(got) and "interface: e1000" in log and "gateway 10.0.2.2  dns 10.0.2.3" in log
    # traffic still flows after the reconfiguration: the gateway answers a ping
    vm.type_string("ping\n")
    time.sleep(4)
    log = open(vm.LOG, errors="replace").read()
    ok = ok and "got ping reply" in log
finally:
    vm.kill()
print("DHCP PASS" if ok else "DHCP FAIL")
sys.exit(0 if ok else 1)
