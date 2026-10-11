#!/usr/bin/env python3
"""The Wi-Fi window against the simulated adapter (`wifigui demo`): it shows, lists networks, selects, takes a password,
fails on a wrong one, connects with the right one, disconnects, joins an open network, scans again.
Geometry comes from the kernel's [wifigui] ui / row lines (never hard-coded); every step is checked against the serial log
and a screenshot is saved to wifigui_shots/ for a look."""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
SHOTS = os.path.join(ROOT, 'wifigui_shots')
os.makedirs(SHOTS, exist_ok=True)
fails = 0

def check(cond, what):
    global fails
    print(("  PASS  " if cond else "  FAIL  ") + what)
    if not cond: fails += 1

def shot(vm, name):
    try:
        vm.dump()
        from PIL import Image
        Image.open(vm.PPM).save(os.path.join(SHOTS, name + ".png"))
    except Exception as e:
        print("  (screenshot failed: %s)" % e)

def ui(vm, name):
    m = re.findall(r"\[wifigui\] ui %s=(\d+),(\d+),(\d+),(\d+)" % name, vm.serial())
    if not m: return None
    x, y, w, h = map(int, m[-1])
    return x + w // 2, y + h // 2

def row(vm, ssid):
    m = re.findall(r"\[wifigui\] row \d+ (\d+),(\d+),(\d+),(\d+) %s\r?\n" % re.escape(ssid), vm.serial())
    if not m: return None
    x, y, w, h = map(int, m[-1])
    return x + 200, y + 30                       # the name/detail area of the row

def click(vm, pos, expect):
    return pos is not None and vm.click_screen(pos[0], pos[1], expect)

def state(vm):
    m = re.findall(r"\[wifigui\] state (\w+)", vm.serial())
    return m[-1] if m else None

vm = OkVM("wifigui")
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("wifigui demo\n")
    check(vm.wait_for("[wifigui] open (demo)", timeout=20), "window opens")
    check(vm.wait_for("row 0 ", timeout=20), "networks are listed after the scan")
    time.sleep(1)
    shot(vm, "01_list")
    check(row(vm, "Home-Fiber") is not None and row(vm, "CoffeeShop Guest") is not None, "rows for the demo networks")

    # the last network is below the fold: the wheel scrolls the list
    check(row(vm, "WPA3-Only-Net") is not None, "the last row exists")
    vm.premove(*row(vm, "Home-Fiber"))
    for _ in range(4):
        vm.mon("mouse_move 0 0 -1", 0.3)
    time.sleep(1.5); shot(vm, "01b_scrolled")
    check(row(vm, "WPA3-Only-Net")[1] < 820, "the wheel scrolled the list up")

    # unsupported network: selecting shows a note; select again to collapse
    check(click(vm, row(vm, "WPA3-Only-Net"), r"\[wifigui\] select WPA3-Only-Net"), "select a WPA3 network")
    time.sleep(1); shot(vm, "02_unsupported")
    click(vm, row(vm, "WPA3-Only-Net"), r"\[wifigui\] select WPA3-Only-Net")
    for _ in range(6):                                            # back to the top
        vm.mon("mouse_move 0 0 1", 0.3)
    time.sleep(1.5)
    check(row(vm, "Home-Fiber")[1] < 400, "the wheel scrolls back to the top")

    # wrong password -> failure
    check(click(vm, row(vm, "Home-Fiber"), r"\[wifigui\] select Home-Fiber\r?\n"), "select Home-Fiber")
    time.sleep(1)
    check(ui(vm, "field") is not None and ui(vm, "connect") is not None, "password field and Connect button appear")
    vm.type_string("wrongpass1")
    time.sleep(1); shot(vm, "03_typing")
    check(click(vm, ui(vm, "connect"), r"\[wifigui\] connect Home-Fiber\r?\n"), "Connect pressed")
    time.sleep(0.5); shot(vm, "04_connecting")
    check(vm.wait_for("state failed", timeout=15), "a wrong password ends in a failure")
    time.sleep(1); shot(vm, "05_failed")
    check(click(vm, ui(vm, "disconnect"), r"\[wifigui\] disconnect"), "Dismiss clears the failure")
    time.sleep(1)

    # right password via Enter
    check(click(vm, row(vm, "Home-Fiber"), r"\[wifigui\] select Home-Fiber\r?\n"), "select Home-Fiber again")
    vm.type_string("demo1234\n")
    check(vm.wait_for("state up ssid=Home-Fiber", timeout=15), "the right password connects (Enter)")
    time.sleep(1.5); shot(vm, "06_connected")
    check("ip=192.168.50.23" in vm.serial(), "the address is shown")
    check(click(vm, ui(vm, "disconnect"), r"\[wifigui\] disconnect"), "Disconnect")
    time.sleep(1)
    check(state(vm) == "idle", "back to idle")

    # an open network joins without a password
    check(click(vm, row(vm, "CoffeeShop Guest"), r"\[wifigui\] select CoffeeShop Guest"), "select the open network")
    time.sleep(1); shot(vm, "07_open_select")
    check(click(vm, ui(vm, "connect"), r"\[wifigui\] connect CoffeeShop Guest"), "Connect (no password)")
    check(vm.wait_for("state up ssid=CoffeeShop Guest", timeout=15), "joined")
    time.sleep(1.5); shot(vm, "08_open_connected")
    click(vm, ui(vm, "disconnect"), r"\[wifigui\] disconnect")
    time.sleep(1)

    # scanning again
    check(click(vm, ui(vm, "scan"), r"\[wifigui\] scan"), "Scan pressed")
    time.sleep(0.4); shot(vm, "09_scanning")
    time.sleep(2.5); shot(vm, "10_scanned")
finally:
    log = vm.serial()
    vm.kill()
    bad = [l for l in log.splitlines() if "EXCEPTION" in l.upper() or "PAGE FAULT" in l.upper() or "kmalloc FAIL" in l]
    check(not bad, "no faults in the kernel log %s" % bad[:3])
print("WIFIGUI PASS" if not fails else "WIFIGUI FAIL (%d)" % fails)
sys.exit(1 if fails else 0)
