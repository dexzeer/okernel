#!/usr/bin/env python3
"""Boot the desktop, screenshot, and confirm text pixels actually render
(dark pixels present on the light terminal background). Guards against a
regression where the anti-aliased glyph draw produced empty/invisible text.
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM
from PIL import Image
from io import BytesIO

vm = OkVM("fontcheck")
time.sleep(15)

w, h, pix = vm.dump()
ppm_bytes = b"P6\n%d %d\n255\n" % (w, h) + pix
img = Image.open(BytesIO(ppm_bytes)).convert("RGB")
W, H = img.size
px = img.load()

# Count "dark" pixels (text is dark on a light terminal/window background).
# Wallpaper is a light blue/orange gradient, so dark pixels => text/UI chrome.
dark = 0
for y in range(0, H, 3):
    for x in range(0, W, 3):
        r, g, b = px[x, y]
        if r < 90 and g < 90 and b < 90:
            dark += 1

print(f"image {W}x{H}, sampled dark(text) pixels = {dark}")
# A booted desktop with a terminal prompt + chrome should have many dark px.
ok = dark > 200
print("FONT_RENDER_TEST", "PASS" if ok else "FAIL", f"(dark={dark})")
vm.kill()
sys.exit(0 if ok else 1)
