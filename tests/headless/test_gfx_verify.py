#!/usr/bin/env python3
"""Damage-tracking / display-backend self check: after a mix of drawing (typing,
new windows, a browser page, wheel scrolling, window dragging, a mouse wiggle)
compare what is on the screen with the kernel's backbuffer.

  * GRUB framebuffer backend: the kernel's `gfxcheck` flushes and compares the
    framebuffer with the backbuffer (outside the cursor sprite).
  * GPU backends (OKVM_VGA=virtio): the screen cannot be read back by the guest,
    so `gfxsum` logs a hash of every 16-row band of the backbuffer and this test
    hashes the same bands of QEMU's screendump.

Any pixel that was drawn but never marked dirty (or presented from a wrong span)
shows up as a mismatch.

    python3 tests/headless/test_gfx_verify.py
    OKVM_VGA=virtio python3 tests/headless/test_gfx_verify.py
"""
import sys, os, time, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM
import numpy as np

vm = OkVM("gfxverify")
ok = True
gpu = os.environ.get("OKVM_VGA", "std") != "std"

def band_hashes(w, h, raw):
    px = np.frombuffer(raw, dtype=np.uint8, count=w * h * 3).reshape(h, w, 3).astype(np.uint64)
    v = (px[:, :, 0] << np.uint64(16)) | (px[:, :, 1] << np.uint64(8)) | px[:, :, 2]
    nb = (h + 15) // 16
    mul = np.uint64(16777619)
    mask = np.uint64(0xFFFFFFFF)
    hsh = np.full(nb, 2166136261, dtype=np.uint64)
    full = h // 16                                   # bands with all 16 rows
    bands = v[:full * 16].reshape(full, 16 * w)
    hf = hsh[:full]
    for i in range(16 * w):
        hf = ((hf ^ bands[:, i]) * mul) & mask
    hsh[:full] = hf
    if nb > full:                                    # the short last band (1080 = 67*16 + 8)
        last = v[full * 16:].reshape(-1)
        x = 2166136261
        for p_ in last.tolist():
            x = ((x ^ p_) * 16777619) & 0xFFFFFFFF
        hsh[full] = x
    return hsh

def check_gpu(label, tries=3):
    """Hash the backbuffer in the kernel (after the next flush) and the screendump
    on the host. The terminal's text cursor blinks once a second, so a mismatch
    caught mid-blink is retried."""
    global ok
    bad = None
    for attempt in range(tries):
        n0 = len(re.findall(r"\[gfx\] sum done", vm.serial()))
        vm.type_string("gfxsum\n", delay=0.03)
        t0 = time.time()
        while time.time() - t0 < 20 and len(re.findall(r"\[gfx\] sum done", vm.serial())) <= n0:
            time.sleep(0.1)
        w, h, raw = vm.dump()                        # right after the kernel hashed it
        log = vm.serial()
        last = log.rindex("[gfx] sum backend=")
        seg = log[last:]
        want = {int(b): int(x, 16) for b, x in re.findall(r"\[gfx\] blk (\d+) ([0-9a-f]+)", seg)}
        if len(want) != 68 or (w, h) != (1920, 1080):
            print(f"    FAIL  {label}: bad data (blocks={len(want)} dump={w}x{h})", flush=True)
            ok = False
            return
        got = band_hashes(w, h, raw)
        bad = [b for b in range(68) if int(got[b]) != want[b]]
        if not bad:
            break
    print(f"    {'PASS' if not bad else 'FAIL'}  {label}: {len(bad)} of 68 bands differ {bad[:6]}", flush=True)
    ok = ok and not bad

def check_fb(label):
    global ok
    n0 = len(re.findall(r"\[gfx\] verify:", vm.serial()))
    for attempt in range(2):
        # a second attempt opens a fresh (focused) terminal first: under a loaded host QEMU
        # drops queued keys (16-byte PS/2 buffer), and the first try may have typed elsewhere
        if attempt:
            vm.mon("sendkey ctrl-alt-t", 0.3); time.sleep(2)
        vm.type_string("gfxcheck\n", delay=0.03)
        t0 = time.time()
        while time.time() - t0 < 12:
            m = re.findall(r"\[gfx\] verify: (\d+) mismatching pixels \(first at (-?\d+),(-?\d+)\)", vm.serial())
            if len(m) > n0:
                bad, x, y = m[-1]
                good = int(bad) == 0
                print(f"    {'PASS' if good else 'FAIL'}  {label}: {bad} mismatches (first at {x},{y})", flush=True)
                ok = ok and good
                return
            time.sleep(0.5)
    print(f"    FAIL  {label}: gfxcheck did not answer", flush=True)
    ok = False

check = check_gpu if gpu else check_fb

try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    log = vm.serial()
    m = re.search(r"\[gfx\] display backend: (.*)", log)
    print(f"    display backend: {m.group(1) if m else '?'}", flush=True)
    if gpu and (not m or "virtio-gpu" not in m.group(1)):
        print("    FAIL  virtio-gpu backend not active"); ok = False
    check("fresh desktop")
    vm.type_string("help --a\n", delay=0.03); time.sleep(1)
    check("terminal text")
    vm.mon("sendkey ctrl-alt-t", 0.3); time.sleep(2)
    vm.type_string("echo second terminal window\n", delay=0.03); time.sleep(1)
    check("second terminal")

    vm.type_string("okai\n", delay=0.03)
    vm.wait_for("home rendered", timeout=40)
    time.sleep(2)
    vm.mon("sendkey ctrl-alt-t", 0.3); time.sleep(2)     # a terminal above okai to type into
    check("okai home page")

    # wheel over the page (hmp: dz > 0 scrolls up on the real path; both directions)
    vm.premove(700, 500)
    for dz in (1, 1, 1, -1, -1, -1, -1, -1, 1):
        vm.mon(f"mouse_move 0 0 {dz}", 0.25)
    time.sleep(1)
    check("after wheel scroll")

    # drag the terminal by its title bar
    vm.premove(500, 150)
    vm.mon("mouse_button 1", 0.3)
    for _ in range(12):
        vm.mon("mouse_move 14 9", 0.08)
    vm.mon("mouse_button 0", 0.3)
    time.sleep(1)
    vm.mon("sendkey ctrl-alt-t", 0.3); time.sleep(2)       # the drag may have focused the page: fresh terminal
    check("after window drag")

    # many small keystroke updates
    vm.type_string("echo " + "x" * 40 + "\n", delay=0.02)
    for i in range(6):
        vm.type_string(f"echo line {i}\n", delay=0.02)
    time.sleep(1)
    check("after typing")

    # random wiggle: cursor motion must not leave residue in the scene
    for _ in range(10):
        vm.mon("mouse_move 40 25", 0.05)
        vm.mon("mouse_move -40 -25", 0.05)
    time.sleep(1)
    check("after mouse wiggle")

    log = vm.serial()
    bad = re.findall(r"\[ISR\][^\n]*|\[vgpu\][^\n]*(?:failed|timed out)[^\n]*", log)
    if bad:
        print("    FAIL  kernel/driver errors:", bad[:3]); ok = False
finally:
    vm.kill()

print("GFX_VERIFY", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
