#!/usr/bin/env python3
"""Codepoint list for the CJK font subset: the GB2312, JIS X 0208 and
KS X 1001 repertoires (decoded from the host codecs) plus the CJK punctuation,
kana, halfwidth and fullwidth blocks. Prints a fontTools --unicodes= string.

    python3 tools/gen_cjk_codes.py
"""
import sys

cps = set()

def try_decode(codec, lead_lo, lead_hi, trail_lo, trail_hi):
    for lead in range(lead_lo, lead_hi + 1):
        for trail in range(trail_lo, trail_hi + 1):
            try:
                ch = bytes([lead, trail]).decode(codec)
            except (UnicodeDecodeError, ValueError):
                continue
            for c in ch:
                cps.add(ord(c))

try_decode("gb2312", 0xA1, 0xF7, 0xA1, 0xFE)     # GB2312: hanzi + symbols
try_decode("euc_jp", 0xA1, 0xFE, 0xA1, 0xFE)     # JIS X 0208
try_decode("euc_kr", 0xA1, 0xFD, 0xA1, 0xFE)     # KS X 1001 (2350 common hangul)

blocks = [
    (0x0020, 0x007E),   # basic latin (fallback safety)
    (0x00A0, 0x00FF),   # latin-1 punctuation/dashes some pages use
    (0x2010, 0x2027),   # general punctuation (dashes, quotes, ellipsis)
    (0x2E80, 0x2EFF),   # CJK radicals
    (0x3000, 0x303F),   # CJK symbols and punctuation
    (0x3040, 0x30FF),   # hiragana + katakana
    (0x3190, 0x319F),   # kanbun
    (0x31F0, 0x31FF),   # katakana phonetic extensions
    (0x3300, 0x33FF),   # CJK compatibility (square units)
    (0xF900, 0xFAFF),   # CJK compatibility ideographs
    (0xFE10, 0xFE19),   # vertical forms
    (0xFE30, 0xFE4F),   # CJK compatibility forms
    (0xFF00, 0xFFEF),   # halfwidth and fullwidth forms
]
for a, b in blocks:
    for cp in range(a, b + 1):
        cps.add(cp)

out = []
start = prev = None
for cp in sorted(cps):
    if start is None:
        start = prev = cp
        continue
    if cp == prev + 1:
        prev = cp
        continue
    out.append((start, prev))
    start = prev = cp
if start is not None:
    out.append((start, prev))
print(",".join("%X" % a if a == b else "%X-%X" % (a, b) for a, b in out))
sys.stderr.write("%d codepoints\n" % len(cps))
