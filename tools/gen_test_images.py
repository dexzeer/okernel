#!/usr/bin/env python3
"""Generate image-decoder stress inputs into tests/web/out/img/ (not committed).
    python3 tools/gen_test_images.py && python3 tests/web/image_diff.py tests/web/out/img/*
"""
import os
from PIL import Image, ImageDraw

out = 'tests/web/out/img'
os.makedirs(out, exist_ok=True)

def base(w=157, h=93, mode='RGB'):
    im = Image.new('RGB', (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = ((x * 255) // w, (y * 255) // h, ((x ^ y) * 7) & 255)
    d = ImageDraw.Draw(im)
    d.ellipse((20, 10, 90, 80), fill=(250, 40, 30))
    d.rectangle((100, 20, 140, 70), outline=(0, 0, 0), width=3)
    d.text((30, 40), 'okai', fill=(255, 255, 255))
    return im.convert(mode) if mode != 'RGB' else im

im = base()
for sub in (0, 1, 2):  # 4:4:4, 4:2:2, 4:2:0
    im.save('%s/jpg_sub%d.jpg' % (out, sub), quality=90, subsampling=sub)
    im.save('%s/jpg_prog_sub%d.jpg' % (out, sub), quality=85, subsampling=sub, progressive=True)
im.save(out + '/jpg_q20.jpg', quality=20)
im.save(out + '/jpg_restart.jpg', quality=80, restart_marker_blocks=3) if hasattr(Image, 'core') else None
base(mode='L').save(out + '/jpg_gray.jpg', quality=90)
base(mode='L').save(out + '/jpg_gray_prog.jpg', quality=90, progressive=True)
base().convert('CMYK').save(out + '/jpg_cmyk.jpg', quality=90)
base(1000, 600).save(out + '/jpg_big.jpg', quality=85, progressive=True)

base().save(out + '/png_rgb.png')
base().save(out + '/png_rgb_interlaced.png', interlace=True) if False else None
rgba = base().convert('RGBA')
a = rgba.load()
for y in range(rgba.height):
    for x in range(rgba.width):
        r, g, b, _ = a[x, y]
        a[x, y] = (r, g, b, (x * 2) & 255)
rgba.save(out + '/png_rgba.png')
base(mode='L').save(out + '/png_gray.png')
base(mode='LA').save(out + '/png_graya.png')
pal = base().convert('P', palette=Image.ADAPTIVE, colors=16)
pal.save(out + '/png_pal4.png', bits=4)
pal.info['transparency'] = 3
pal.save(out + '/png_pal_trns.png', transparency=3)
base().convert('1').save(out + '/png_1bit.png')
base(mode='I;16').save(out + '/png_16bit.png') if False else None
im16 = Image.new('I;16', (64, 40))
p16 = im16.load()
for y in range(40):
    for x in range(64):
        p16[x, y] = x * 1000 + y * 50
im16.save(out + '/png_gray16.png')

g = base().convert('P', palette=Image.ADAPTIVE, colors=64)
g.save(out + '/gif_basic.gif')
g.save(out + '/gif_interlaced.gif', interlace=True)
g.info['transparency'] = 5
g.save(out + '/gif_trns.gif', transparency=5)
base().save(out + '/bmp24.bmp')
print('generated into', out)
