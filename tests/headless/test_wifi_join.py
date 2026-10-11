#!/usr/bin/env python3
"""Real-adapter test: scan, join the network named in wifi_secret.txt (line 1 = name, line 2 = pass phrase; git-ignored), get an
address by DHCP, ping the gateway, resolve a name. Run through tools/wifi/run_usb_test.ps1 -Script test_wifi_join.py.
The pass phrase is only ever typed into the VM; it is never printed or logged."""
import os, sys, time
sys.path.insert(0, 'tests/headless')
from okvm import OkVM

ssid, psk = open(os.path.join(os.path.dirname(__file__), '..', '..', 'wifi_secret.txt')).read().split('\n')[:2]
vm = OkVM("wifijoin")
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("wifi start\n")
    started = vm.wait_for("wifi: started", timeout=240)
    time.sleep(2)
    if started:
        vm.type_string("wifi scan\n"); time.sleep(10)
        vm.type_string("wifi list\n"); time.sleep(3)
        vm.type_string("wifi connect " + ssid + " " + psk + "\n")
        vm.wait_for("wifi: connected to", timeout=40, fail_pats=["refused", "did not finish", "no answer", "cannot join"])
        time.sleep(1)
        vm.wait_for("dhcp: address", timeout=40)
        time.sleep(2)
        vm.type_string("wifi status\n"); time.sleep(3)
        vm.type_string("ipconfig\n"); time.sleep(3)
        vm.type_string("ping\n"); time.sleep(5)
        vm.type_string("resolve example.com\n"); time.sleep(8)
        vm.type_string("wifi status\n"); time.sleep(3)
        vm.type_string("wifi log\n"); time.sleep(3)
finally:
    log = open(vm.LOG, errors="replace").read()
    vm.kill()
    out = [l for l in log.splitlines() if not l.startswith("[fps]")]
    print("\n".join(out[-120:]))
