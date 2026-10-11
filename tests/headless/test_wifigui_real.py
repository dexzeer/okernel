#!/usr/bin/env python3
"""The Wi-Fi window with the REAL adapter (run through tools/wifi/run_usb_test.ps1 -Script test_wifigui_real.py):
Turn on -> scan -> pick the network from wifi_secret.txt -> type its password -> connect -> address. Screenshots go to
wifigui_shots/ (the password field shows dots; the password is never printed)."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
SHOTS = os.path.join(ROOT, 'wifigui_shots')
os.makedirs(SHOTS, exist_ok=True)
ssid, psk = open(os.path.join(ROOT, 'wifi_secret.txt')).read().split('\n')[:2]

def shot(vm, name):
    vm.dump()
    from PIL import Image
    Image.open(vm.PPM).save(os.path.join(SHOTS, "real_" + name + ".png"))

def ui(vm, name):
    m = re.findall(r"\[wifigui\] ui %s=(\d+),(\d+),(\d+),(\d+)" % name, vm.serial())
    if not m: return None
    x, y, w, h = map(int, m[-1]); return x + w // 2, y + h // 2

def row(vm, name):
    m = re.findall(r"\[wifigui\] row \d+ (\d+),(\d+),(\d+),(\d+) %s\r?\n" % re.escape(name), vm.serial())
    if not m: return None
    x, y, w, h = map(int, m[-1]); return x + 200, y + 30

vm = OkVM("wifiguireal")
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("wifigui\n")
    vm.wait_for("[wifigui] open", timeout=20)
    time.sleep(2); shot(vm, "01_turn_on")
    p = ui(vm, "power")
    vm.click_screen(p[0], p[1], r"\[wifigui\] adapter start result|\[wifigui\] select")
    time.sleep(1); shot(vm, "02_starting")
    vm.wait_for("adapter start result 0", timeout=240)
    vm.wait_for("row 0 ", timeout=120)
    time.sleep(8); shot(vm, "03_networks")
    r = row(vm, ssid)
    print("network row found:", r is not None)
    if r:
        vm.click_screen(r[0], r[1], r"\[wifigui\] select " + re.escape(ssid) + r"\r?\n")
        time.sleep(1)
        vm.type_string(psk)
        time.sleep(1); shot(vm, "04_password")
        vm.type_string("\n")
        vm.wait_for("state up", timeout=60, fail_pats=["state failed"])
        time.sleep(3); shot(vm, "05_connected")
        print("state:", re.findall(r"\[wifigui\] state (\w+)", vm.serial())[-3:])
finally:
    log = vm.serial()
    vm.kill()
    out = [l for l in log.splitlines() if ("wifigui" in l and "row" not in l and "ui " not in l) or l.startswith("[w]")]
    print("\n".join(out[-40:]))
