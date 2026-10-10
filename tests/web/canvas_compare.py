#!/usr/bin/env python3
"""Render a canvas page with wbrowse and with headless Edge and report the pixel differences.

    python3 tests/web/canvas_compare.py tests/web/js/canvas_scene.html [w h]

Run under WSL (it reaches the Windows Edge). Writes tests/web/out/canvas_<name>_{ours,edge}.png and a
side-by-side with the difference.
"""
import os, subprocess, sys
import numpy as np
from PIL import Image

EDGE = '/mnt/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
page = sys.argv[1]
w = int(sys.argv[2]) if len(sys.argv) > 2 else 400
h = int(sys.argv[3]) if len(sys.argv) > 3 else 300
name = os.path.splitext(os.path.basename(page))[0]
out = 'tests/web/out'
os.makedirs(out, exist_ok=True)
ppm = f'{out}/canvas_{name}_ours.ppm'
subprocess.run(['./build-host/wbrowse', page, str(w), str(h), ppm, '5'], capture_output=True, text=True, timeout=120)
ours = Image.open(ppm).convert('RGB')
win = subprocess.run(['wslpath', '-w', os.path.abspath(page)], capture_output=True, text=True).stdout.strip()
shot = os.path.abspath(f'{out}/canvas_{name}_edge.png')
winshot = subprocess.run(['wslpath', '-w', shot], capture_output=True, text=True).stdout.strip()
subprocess.run([EDGE, '--headless=new', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1', f'--window-size={w},{h}',
                f'--screenshot={winshot}', 'file:///' + win.replace(chr(92), '/')], capture_output=True, timeout=120)
edge = Image.open(shot).convert('RGB').crop((0, 0, w, h))
ours = ours.crop((0, 0, w, h))
a = np.asarray(ours).astype(int); b = np.asarray(edge).astype(int)
d = np.abs(a - b).max(axis=2)
print(f'{name}: mean abs diff {np.abs(a - b).mean():.3f}, pixels differing by >32: {(d > 32).sum()} of {d.size}, >8: {(d > 8).sum()}, max {d.max()}')
ours.save(f'{out}/canvas_{name}_ours.png')
side = Image.new('RGB', (w * 3, h))
side.paste(ours, (0, 0)); side.paste(edge, (w, 0)); side.paste(Image.fromarray((255 - np.clip(d * 4, 0, 255)).astype('uint8')).convert('RGB'), (w * 2, 0))
side.save(f'{out}/canvas_{name}_cmp.png')
