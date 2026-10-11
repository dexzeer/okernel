#!/usr/bin/env python3
"""Real-adapter test: join the network in wifi_secret.txt, then load web pages through the Wi-Fi link (HTTP, then HTTPS)."""
import os, sys, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
ssid, psk = open(os.path.join(root, 'wifi_secret.txt')).read().split('\n')[:2]
vm = OkVM("wifiweb")
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("wifi start\n")
    vm.wait_for("wifi: started", timeout=240)
    time.sleep(2)
    vm.type_string("wifi scan\n"); vm.wait_for("scan finished", timeout=120); time.sleep(2)
    vm.type_string("wifi connect " + ssid + " " + psk + "\n")
    vm.wait_for("wifi: connected to", timeout=40, fail_pats=["refused", "did not finish", "no answer", "cannot join"])
    vm.wait_for("dhcp: address", timeout=40)
    time.sleep(3)
    for i, url in enumerate(["https://example.com/"]):
        before = vm.serial().count("parse: count=")
        vm.type_string("okai " + url + "\n")
        t0 = time.time()
        while time.time() - t0 < 100:
            log = vm.serial()
            if log.count("parse: count=") > before or "[okai] error page:" in log[len(log) - 4000:]:
                break
            time.sleep(2)
        time.sleep(6)
        try:
            vm.dump()
            from PIL import Image
            Image.open(vm.PPM).save(os.path.join(root, "wifi_web_%d.png" % i))
        except Exception as e:
            print("screenshot failed:", e)
    vm.type_string("wifi status\n"); time.sleep(3)
finally:
    log = open(vm.LOG, errors="replace").read()
    vm.kill()
    keep = [l for l in log.splitlines() if re.search(r"okai|tls|TLS|https|parse|fetch|wifi|dhcp|dns|tcp|link|frames", l) and "[fps]" not in l]
    print("\n".join(keep[-80:]))
