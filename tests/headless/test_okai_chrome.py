#!/usr/bin/env python3
"""okai chrome end to end (Firefox-style tab strip + nav bar, okai_ui.c).

Aims every click from the kernel's `[okai] ui name=x,y,w,h` geometry lines
(vm.click_ui) and checks:
  - hover: the reload button gets its hover background
  - address bar: click focuses it (focus ring), typing + Enter navigates
  - identity box: opens / closes the security panel (white card pixels)
  - '+' opens a tab, its x closes it
  - caption buttons: maximize/restore toggle (geometry follows), minimize +
    taskbar restore, and the top-right corner is chrome (no FPS box over it)
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR
from PIL import Image

FX_BTN_HOVER = (0xE0, 0xE0, 0xE6)
FX_FOCUS = (0x00, 0x61, 0xE0)

def shot(vm, name):
    p = os.path.join(OUTDIR, "chrome_%s.ppm" % name)
    vm.dump(p)
    return Image.open(p).convert("RGB")

def near(a, b, tol=6):
    return all(abs(x - y) <= tol for x, y in zip(a, b))

results = []
def check(name, ok, info=""):
    print(f"{name}: {'OK' if ok else 'FAIL'} {info}", flush=True)
    results.append(ok)

vm = OkVM("chrome")
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("okai https://example.com/\n")
    check("load", vm.wait_for("https parse: count=", timeout=120))
    time.sleep(3)

    # top-right corner: caption buttons, not the old black FPS box
    close = vm.ui_rect("close")
    im = shot(vm, "base")
    corner = im.getpixel((close[0] + 4, close[1] + 4))
    check("caption corner is chrome", corner[0] > 200 and corner[1] > 200, str(corner))

    # hover on reload
    rx, ry = vm.ui_center("reload")
    vm.anchor(300, 400)            # blank page area
    vm.premove(rx, ry)
    time.sleep(1.5)
    r = vm.ui_rect("reload")
    im = shot(vm, "hover")
    hv = im.getpixel((r[0] + 4, r[1] + r[3] // 2))
    check("reload hover", near(hv, FX_BTN_HOVER), str(hv))

    # address bar focus + navigate
    ux, uy = vm.ui_center("url")
    ok = vm.click_screen(ux + 200, uy, r"address bar focused")
    time.sleep(1)
    u = vm.ui_rect("url")
    im = shot(vm, "focus")
    ring = im.getpixel((u[0] + u[2] // 2, u[1]))
    check("address focus ring", ok and near(ring, FX_FOCUS, 40), str(ring))
    n0 = vm.serial().count("parse: count=")
    vm.type_string("http://example.com/\n")
    end = time.time() + 90
    while time.time() < end and vm.serial().count("parse: count=") <= n0:
        time.sleep(1)
    check("address bar navigate", vm.serial().count("parse: count=") > n0)
    time.sleep(2)

    # security panel
    opened = vm.click_ui("ident", r"security popup open")
    time.sleep(1.5)
    i = vm.ui_rect("ident")
    im = shot(vm, "panel")
    white = sum(1 for y in range(i[1] + 50, i[1] + 190, 3) for x in range(i[0] + 10, i[0] + 360, 3)
                if im.getpixel((x, y)) == (255, 255, 255))
    closed = vm.click_ui("ident", r"security popup closed")
    time.sleep(1.5)
    im = shot(vm, "panel_closed")
    white2 = sum(1 for y in range(i[1] + 50, i[1] + 190, 3) for x in range(i[0] + 10, i[0] + 360, 3)
                 if im.getpixel((x, y)) == (255, 255, 255))
    check("security panel", opened and closed and white > 3000 and white2 < 300,
          f"white={white} after={white2}")

    # new tab + close it
    homes = vm.serial().count("home rendered")
    added = vm.click_ui("newtab", r"nav action=5")
    time.sleep(2)
    added = added and vm.serial().count("home rendered") > homes
    end = time.time() + 15
    while time.time() < end and not vm.ui_rect("tab1"):
        time.sleep(0.5)
    shot(vm, "twotabs")
    check("new tab", added and vm.ui_rect("tab1") is not None)
    closedt = vm.click_ui("tabx1", r"\[okai\] closing tab 1")
    check("close tab", closedt)
    time.sleep(2)

    # maximize toggle: restore -> smaller window, geometry re-logged
    url0 = vm.ui_rect("url")
    mx_, my_ = vm.ui_center("max")
    restored = vm.click_ui("max", r"\[win\] \d+ restored", anchor=(mx_ - 400, my_))
    time.sleep(3)
    url1 = vm.ui_rect("url")
    shot(vm, "restored")
    check("restore", restored and url1 and url1 != url0, f"{url0} -> {url1}")
    mx_, my_ = vm.ui_center("max")
    maxed = vm.click_ui("max", r"\[win\] \d+ maximized", anchor=(mx_ - 300, my_))
    time.sleep(3)
    check("maximize", maxed and vm.ui_rect("url") == url0, str(vm.ui_rect("url")))

    # minimize, then restore from okai's taskbar button ([taskbar] btn log)
    nx_, ny_ = vm.ui_center("min")
    mini = vm.click_ui("min", r"\[win\] \d+ minimized", anchor=(nx_ - 300, ny_))
    time.sleep(1.5)
    wid = int(re.findall(r"\[win\] (\d+) minimized", vm.serial())[-1])
    tb = vm.taskbar_rect(wid)
    vm.anchor(1200, 1080 - 22)     # empty taskbar
    back = bool(tb) and vm.click_screen(tb[0] + tb[2] // 2, tb[1] + tb[3] // 2,
                                        r"restored from taskbar")
    time.sleep(2)
    im = shot(vm, "final")
    check("minimize + taskbar restore", mini and back)
finally:
    vm.kill()

faults = [l for l in vm.serial().splitlines()
          if "PAGE FAULT" in l.upper() or "[ISR] Exception" in l or "kmalloc FAIL" in l]
check("no faults", not faults, str(faults[:3]))
ok = all(results)
print("CHROME_TEST", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
