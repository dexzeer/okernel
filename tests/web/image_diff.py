#!/usr/bin/env python3
"""Differential test of the web engine's image decoders against PIL.

Decodes every PNG/JPEG/GIF/BMP under tests/web/corpus with build-host/imgdump
and with PIL, and compares dimensions and mean absolute RGBA error.
JPEG decoders legitimately differ by a few levels (IDCT/upsampling), so the
JPEG threshold is looser.
"""
import glob, os, subprocess, sys, tempfile
from PIL import Image

def kind(data):
    if data[:8] == b'\x89PNG\r\n\x1a\n': return 'png'
    if data[:2] == b'\xff\xd8': return 'jpeg'
    if data[:4] == b'GIF8': return 'gif'
    if data[:2] == b'BM': return 'bmp'
    return None

def main():
    files = sorted(glob.glob('tests/web/corpus/*/*'))
    extra = [f for f in sys.argv[1:]]
    ok = bad = 0
    for path in files + extra:
        if path.endswith(('.html', '.css', '.json', '.tsv')):
            continue
        data = open(path, 'rb').read()
        k = kind(data)
        if not k:
            continue
        try:
            ref = Image.open(path)
            ref.seek(0)
            if ref.mode.startswith('I'):  # 16-bit gray: browsers take the high byte
                ref = ref.point(lambda v: v / 256).convert('L')
            ref = ref.convert('RGBA')
        except Exception as e:
            print('SKIP (PIL cannot decode)', path, e)
            continue
        with tempfile.NamedTemporaryFile(delete=False) as t:
            out = t.name
        r = subprocess.run(['./build-host/imgdump', path, out], capture_output=True, text=True)
        if not r.stdout.startswith('OK'):
            print('FAIL decode', k, path)
            bad += 1
            continue
        raw = open(out, 'rb').read()
        os.unlink(out)
        nl = raw.index(b'\n')
        w, h = map(int, raw[:nl].split())
        px = raw[nl + 1:]
        if (w, h) != ref.size:
            print('FAIL size', k, path, (w, h), ref.size)
            bad += 1
            continue
        rp = ref.tobytes()
        n = len(rp)
        # mean abs diff over premultiplied-ish (ignore color under alpha 0)
        tot = 0
        cnt = 0
        step = max(1, (n // 4) // 200000)
        for i in range(0, n, 4 * step):
            a1, a2 = rp[i + 3], px[i + 3]
            tot += abs(a1 - a2)
            if a1 and a2:
                tot += abs(rp[i] - px[i]) + abs(rp[i + 1] - px[i + 1]) + abs(rp[i + 2] - px[i + 2])
            cnt += 1
        mad = tot / max(1, cnt) / 4
        lim = 6.0 if k == 'jpeg' else 0.5  # jpeg: IDCT + chroma upsampling differ
        status = 'OK  ' if mad <= lim else 'FAIL'
        print('%s %-4s %5dx%-5d mad=%.3f %s' % (status, k, w, h, mad, path))
        if mad <= lim: ok += 1
        else: bad += 1
    print('IMAGE DIFF: %d ok, %d failed' % (ok, bad))
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    main()
