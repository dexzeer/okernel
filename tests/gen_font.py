#!/usr/bin/env python3
"""Regenerate tests/font_data.h from the kernel font tables.

The preview links the REAL main font (src/font_data.c, Terminus 16x32)
directly, so only the extended table (static inside graphics.c, 8x16
slots for Cyrillic/symbols) is extracted here.
"""
import re
src = open('../src/graphics.c').read()
ext = re.search(r'static const uint8_t font8x16_ext\[0x80\]\[16\] = \{(.*?)\n\};', src, re.S)
assert ext, "font8x16_ext table not found in graphics.c"
out = ["// Generated from src/graphics.c — extended font for the host preview.",
       "// Main font comes from src/font_data.c (linked into the preview).",
       "#ifndef FONT_DATA_H", "#define FONT_DATA_H", "#include <stdint.h>",
       "static const uint8_t font8x16_ext[0x80][16] = {" + ext.group(1) + "\n};",
       "#endif"]
open('font_data.h', 'w').write("\n".join(out))
print("font_data.h regenerated (ext table only)")
