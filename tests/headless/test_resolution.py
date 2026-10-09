#!/usr/bin/env python3
"""The kernel sizes itself from the mode the bootloader set. For several requested
modes (GRUB picks the closest one the adapter offers), build a test ISO, boot it,
and check: the screen size the kernel logged equals the screendump, the desktop is
consistent (gfxcheck / gfxsum), the okai browser fits the screen with the taskbar
visible below it, and nothing faulted.

    python3 tests/headless/test_resolution.py [WxH ...]      default: 1366x768 1024x768 1920x1080 2560x1440
    OKVM_VGA=virtio python3 tests/headless/test_resolution.py 1280x800
"""
import sys, os, re, subprocess, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import okvm
from okvm import OkVM

modes = sys.argv[1:] or ["1366x768", "1024x768", "1920x1080", "2560x1440"]
root = okvm.PROJECT
ok = True

def build(w, h):
    subprocess.run(["make", "-B", "build-res/start.o", f"VID_W={w}", f"VID_H={h}"], cwd=root, check=True, stdout=subprocess.DEVNULL)
    if os.path.exists(os.path.join(root, "kanarchy-res.bin")):
        os.unlink(os.path.join(root, "kanarchy-res.bin"))
    subprocess.run(["make", "kanarchy-res.iso", f"VID_W={w}", f"VID_H={h}"], cwd=root, check=True, stdout=subprocess.DEVNULL)
    return os.path.join(root, "kanarchy-res.iso")

for mode in modes:
    w, h = map(int, mode.split("x"))
    iso = build(w, h)
    okvm.ISO = iso
    vm = OkVM("res_" + mode)
    good = True
    try:
        vm.wait_for("[mem] heap", timeout=60)
        time.sleep(8)
        log = vm.serial()
        m = re.search(r"\[gfx\] screen (\d+)x(\d+)", log)
        sw, sh = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        dw, dh, _ = vm.dump()
        print(f"  requested {mode}: kernel screen {sw}x{sh}, screendump {dw}x{dh}", flush=True)
        if (sw, sh) != (dw, dh) or sw == 0:
            print("    FAIL  screen size mismatch"); good = False
        vm.type_string("okai\n", delay=0.03)
        vm.wait_for("home rendered", timeout=40)
        time.sleep(2)
        # the browser window and the taskbar must both be inside the screen
        tb = re.findall(r"\[taskbar\] btn win=\d+ (\d+),(\d+),(\d+),(\d+)", vm.serial())
        if not tb or any(int(y) + int(hh) > sh or int(x) + int(ww) > sw for x, y, ww, hh in tb):
            print(f"    FAIL  taskbar buttons outside the screen: {tb[-2:]}"); good = False
        ui = re.findall(r"\[okai\] ui .*close=(\d+),(\d+),(\d+),(\d+)", vm.serial())
        if not ui or int(ui[-1][0]) + int(ui[-1][2]) > sw:
            print(f"    FAIL  okai window wider than the screen: {ui[-1:]}"); good = False
        vm.mon("sendkey ctrl-alt-t", 0.3); time.sleep(2)
        if os.environ.get("OKVM_VGA", "std") == "std":
            n0 = len(re.findall(r"\[gfx\] verify:", vm.serial()))
            vm.type_string("gfxcheck\n", delay=0.03)
            t0 = time.time()
            res = None
            while time.time() - t0 < 25 and res is None:
                mm = re.findall(r"\[gfx\] verify: (\d+) mismatching", vm.serial())
                if len(mm) > n0: res = int(mm[-1])
                time.sleep(0.5)
            print(f"    gfxcheck: {res} mismatches")
            if res != 0: good = False
        bad = re.findall(r"\[ISR\][^\n]*|PAGE FAULT[^\n]*|kmalloc FAIL[^\n]*", vm.serial())
        if bad:
            print("    FAIL  kernel errors:", bad[:2]); good = False
    finally:
        vm.kill()
    print(f"  {mode}: {'PASS' if good else 'FAIL'}", flush=True)
    ok = ok and good

print("RESOLUTION", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
