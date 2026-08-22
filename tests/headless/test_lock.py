#!/usr/bin/env python3
"""Verify the HTTPS lock icon click opens a security popup.
Loads https://example.com/ (lock green/secure), drives the QEMU mouse to the
lock icon, toggles it, and captures two screenshots (open/closed parity).

Key detail: the kernel hit-tests on the BUTTON-PRESS position ([mse] btn=1),
and the cursor drifts leftward after bursts settle. So we (a) track the press
position, not the release, and (b) stop bursting and let the cursor settle
before the action click so press == settled position.
"""
import sys, os, time, zlib, struct, binascii, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

TARGET_X, TARGET_Y = 1212, 100  # aim right of lock center; ~12px leftward
                            # drift during settle lands the click inside [1184,1204]

def ppm_to_png(ppm, png):
    with open(ppm, 'rb') as f: data = f.read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    raw = parts[3][:w * h * 3]
    def chunk(typ, payload):
        return (struct.pack(">I", len(payload)) + typ + payload +
                struct.pack(">I", binascii.crc32(typ + payload) & 0xffffffff))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    stride = w * 3
    rows = b''
    for y in range(h):
        rows += b'\x00' + raw[y * stride:(y + 1) * stride]
    idat = zlib.compress(rows, 9)
    out = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
           chunk(b'IDAT', idat) + chunk(b'IEND', b''))
    with open(png, 'wb') as f: f.write(out)

def last_press(log):
    if isinstance(log, bytes): log = log.decode('latin1')
    pos = None
    for line in log.splitlines():
        m = re.search(r'\[mse\] btn=1 x=(\d+) y=(\d+)', line)
        if m: pos = (int(m.group(1)), int(m.group(2)))
    return pos

def last_dbg(log):
    if isinstance(log, bytes): log = log.decode('latin1')
    return [l for l in log.splitlines() if '[dbg]' in l]

vm = OkVM("lock")
time.sleep(14)
# Navigate via the terminal (proven to work) so the page is https://example.com
# with a green lock. We avoid typing into the in-browser address bar here.
vm.type_string("okai https://example.com/\n")
time.sleep(14)

# Blind shove toward top-right wallpaper (no clicks yet).
for _ in range(15): vm.burst(0, -50)
for _ in range(15): vm.burst(60, 0)
time.sleep(0.5)

# Converge in SAFE stages so we never click the nav buttons (which live at
# x < 1183, left of the address bar) — clicking 'home' there would revert the
# page. The lock lives at x ~1184-1204, so we approach from the right and keep
# every probe click at x >= 1190 (address bar / lock / toolbar, never nav).
NAV_SAFE = 1190  # only click when press x >= this

def probe():
    vm.click()
    return last_press(vm.serial())

# Stage 1: bring X to ~1300 while parked at y~0 (wallpaper, always safe).
for _ in range(40):
    pos = probe()
    if not pos: time.sleep(0.2); continue
    dx = 1300 - pos[0]
    if abs(dx) <= 10: break
    vm.burst(max(-50, min(50, round(dx / 3.75))), 0)
    time.sleep(0.12)

# Stage 2: bring Y to TARGET_Y while held at x=1300 (wallpaper/toolbar, safe).
for _ in range(40):
    pos = probe()
    if not pos: time.sleep(0.2); continue
    dy = TARGET_Y - pos[1]
    if abs(dy) <= 4: break
    vm.burst(0, max(-50, min(50, round(dy / 3.75))))
    time.sleep(0.12)

# Stage 3: bring X to TARGET_X at y=TARGET_Y. Approach from the right; if a
# press lands left of NAV_SAFE, burst right to recover (never click nav).
for _ in range(40):
    pos = probe()
    if not pos: time.sleep(0.2); continue
    if pos[0] < NAV_SAFE:
        vm.burst(25, 0); time.sleep(0.12); continue   # escaped into nav zone
    dx = TARGET_X - pos[0]
    dy = TARGET_Y - pos[1]
    if abs(dx) <= 6 and abs(dy) <= 6:
        print(f"converged(press) at pos={pos}")
        break
    vm.burst(max(-50, min(50, round(dx / 3.75))), 0)
    time.sleep(0.12)
else:
    print("WARN: did not converge; last press=", last_press(vm.serial()))
    vm.kill(); sys.exit(2)

def popup_open(path):
    """True if the security card (white rounded rect) is visible in the shot."""
    from PIL import Image
    im = Image.open(path).convert("RGB"); px = im.load()
    white = 0
    for y in range(123, 224):
        for x in range(1182, 1482):
            r, g, b = px[x, y]
            if r > 248 and g > 248 and b > 248:
                white += 1
    return white > 4000

LOCK_A = os.path.expanduser("~/okvm/lock_a.png")
LOCK_B = os.path.expanduser("~/okvm/lock_b.png")

# Mouse is on the lock. Click until the popup is VISUALLY open (white card in
# the screenshot) — robust against serial-parity drift — then capture shot A.
def last_press_coord(log):
    if isinstance(log, bytes): log = log.decode('latin1')
    m = None
    for line in log.splitlines():
        mm = re.search(r'\[mse\] btn=1 x=(\d+) y=(\d+)', line)
        if mm: m = (int(mm.group(1)), int(mm.group(2)))
    return m

def in_box(pc):
    return pc and 1184 <= pc[0] <= 1204 and 93 <= pc[1] <= 113

# Closed-loop alignment: the cursor drifts unpredictably during settle, so we
# click, read the ACTUAL press coordinate, and burst-correct toward the lock
# center (1194,100) using the same scaling as the convergence. A click whose
# press lands inside the lock hit-box toggles the popup; we verify it opened
# visually (PIL) before capturing shot A.
for i in range(16):
    vm.click(); time.sleep(0.35)
    pc = last_press_coord(vm.serial())
    print(f"click {i}: press={pc}")
    if in_box(pc):
        vm.dump(); ppm_to_png(vm.PPM, LOCK_A)
        if popup_open(LOCK_A):
            print("popup OPEN captured"); break
        # toggled but not open (was already open) -> next click re-opens it
    if pc is None:
        time.sleep(0.2); continue
    dx = max(-8, min(8, round((1194 - pc[0]) / 3.75)))
    dy = max(-8, min(8, round((100 - pc[1]) / 3.75)))
    if dx or dy:
        vm.burst(dx, dy); time.sleep(0.18)
else:
    print("WARN: never landed a click in the lock box")

# Dismiss: one more click closes the popup; capture shot B (closed).
vm.click(); time.sleep(0.8)
vm.dump(); ppm_to_png(vm.PPM, LOCK_B)
print("saved", LOCK_A, "and", LOCK_B)
print("lock_a open:", popup_open(LOCK_A), " lock_b open:", popup_open(LOCK_B))
log = vm.serial()
open("/home/notdexy/okvm/lock_serial.txt", "w").write(log)
vm.kill()
