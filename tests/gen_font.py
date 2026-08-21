#!/usr/bin/env python3
"""Regenerate tests/font_data.h from the kernel font tables in graphics.c."""
import re
src = open('../src/graphics.c').read()
m = re.search(r'static const uint8_t font8x16\[\]\[16\] = \{(.*?)\n\};', src, re.S)
ext = re.search(r'static const uint8_t font8x16_ext\[0x80\]\[16\] = \{(.*?)\n\};', src, re.S)
out = ["// Generated from src/graphics.c — font data for the host preview tool.",
       "#ifndef FONT_DATA_H", "#define FONT_DATA_H", "#include <stdint.h>",
       "static const uint8_t font8x16[][16] = {" + m.group(1) + "\n};",
       "static const uint8_t font8x16_ext[0x80][16] = {" + ext.group(1) + "\n};",
       "#endif"]
open('font_data.h','w').write("\n".join(out))
print("font_data.h regenerated")
