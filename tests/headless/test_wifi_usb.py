#!/usr/bin/env python3
"""Run the Wi-Fi driver against a REAL Realtek USB adapter passed through to QEMU (OKVM_EXTRA names the usb-host device).

Not part of the normal suite: tools/wifi/run_usb_test.ps1 sets the adapter up (usbipd), calls this and gives the adapter
back to Windows. The serial log (devlog tag 'w' plus the output of the wifi shell command) is the result.
usage: test_wifi_usb.py [tag] [seconds-of-scan]
"""
import sys, time
sys.path.insert(0, 'tests/headless')
from okvm import OkVM

tag = sys.argv[1] if len(sys.argv) > 1 else "wifiusb"
scan_s = int(sys.argv[2]) if len(sys.argv) > 2 else 12
vm = OkVM(tag)
ok = True
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("usb\n"); time.sleep(2)
    vm.type_string("wifi start\n")
    started = vm.wait_for("wifi: started", timeout=180) or vm.wait_for("start failed", timeout=5)
    time.sleep(2)
    vm.type_string("wifi status\n"); time.sleep(3)
    if started:
        vm.type_string("wifi scan\n"); time.sleep(scan_s)
        vm.type_string("wifi list\n"); time.sleep(3)
        vm.type_string("wifi status\n"); time.sleep(3)
    vm.type_string("wifi log\n"); time.sleep(3)
finally:
    log = open(vm.LOG, errors="replace").read()
    vm.kill()
    print(log[-12000:])
sys.exit(0)
