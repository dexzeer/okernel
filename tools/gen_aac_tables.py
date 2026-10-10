#!/usr/bin/env python3
"""Generate src/media/aac_tables.c (the AAC-LC constant tables).

  python tools/gen_aac_tables.py <faad2/libfaad dir> > src/media/aac_tables.c

The spectral Huffman codebooks 1-11 and the scalefactor codebook are the one thing in AAC that
cannot be derived: ISO/IEC 14496-3 lists them as tables.  We read them out of a reference decoder
(FAAD2's libfaad/codebook/*.h, read-only, never copied into the tree): tools/aac_dump_hcb.c is a
tiny harness that walks FAAD2's lookup structures and prints every codeword as
"codebook length code values..."; this script validates that dump (prefix-free, every index
present exactly once, lengths/codes consistent) and writes our own tables, indexed by the
spec's codeword index, i.e. a pair/quad of values packed as a mixed-radix number.

The scalefactor band offsets (14496-3 tables 4.6.x...) and the TNS limits are plain data
and are listed below.
"""
import os
import subprocess
import sys
import tempfile

# (values per codeword, modulus, offset): value = digit - offset, digits are most significant first
CB = {
    1: (4, 3, 1), 2: (4, 3, 1), 3: (4, 3, 0), 4: (4, 3, 0),
    5: (2, 9, 4), 6: (2, 9, 4), 7: (2, 8, 0), 8: (2, 8, 0),
    9: (2, 13, 0), 10: (2, 13, 0), 11: (2, 17, 0),
}

SWB_LONG = {  # sample-rate class -> band offsets of a 1024 window (num_swb + 1 entries)
    96: [0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 156, 172, 188, 212, 240, 276, 320, 384, 448, 512, 576, 640, 704, 768, 832, 896, 960, 1024],
    64: [0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 100, 112, 124, 140, 156, 172, 192, 216, 240, 268, 304, 344, 384, 424, 464, 504, 544, 584, 624, 664, 704, 744, 784, 824, 864, 904, 944, 984, 1024],
    48: [0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292, 320, 352, 384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 1024],
    32: [0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292, 320, 352, 384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 960, 992, 1024],
    24: [0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 52, 60, 68, 76, 84, 92, 100, 108, 116, 124, 136, 148, 160, 172, 188, 204, 220, 240, 260, 284, 308, 336, 364, 396, 432, 468, 508, 552, 600, 652, 704, 768, 832, 896, 960, 1024],
    16: [0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 100, 112, 124, 136, 148, 160, 172, 184, 196, 212, 228, 244, 260, 280, 300, 320, 344, 368, 396, 424, 456, 492, 532, 572, 616, 664, 716, 772, 832, 896, 960, 1024],
    8: [0, 12, 24, 36, 48, 60, 72, 84, 96, 108, 120, 132, 144, 156, 172, 188, 204, 220, 236, 252, 268, 288, 308, 328, 348, 372, 396, 420, 448, 476, 508, 544, 580, 620, 664, 712, 764, 820, 880, 944, 1024],
}
SWB_SHORT = {  # offsets of a 128 window
    96: [0, 4, 8, 12, 16, 20, 24, 32, 40, 48, 64, 92, 128],
    64: [0, 4, 8, 12, 16, 20, 24, 32, 40, 48, 64, 92, 128],
    48: [0, 4, 8, 12, 16, 20, 28, 36, 44, 56, 68, 80, 96, 112, 128],
    24: [0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 64, 76, 92, 108, 128],
    16: [0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 60, 72, 88, 108, 128],
    8: [0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 60, 72, 88, 108, 128],
}
# sampling_frequency_index -> (long class, short class)
SR_INDEX = [(96, 96), (96, 96), (64, 64), (48, 48), (48, 48), (32, 48),
            (24, 24), (24, 24), (16, 16), (16, 16), (16, 16), (8, 8)]
SAMPLE_RATES = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350]
NUM_SWB_LONG = [41, 41, 47, 49, 49, 51, 47, 47, 43, 43, 43, 40]       # 14496-3 4.5.4 (checked against the offsets below)
NUM_SWB_SHORT = [12, 12, 12, 14, 14, 14, 15, 15, 15, 15, 15, 15]
# TNS: highest scalefactor band a filter may reach (long, short window) per sampling_frequency_index
TNS_MAX = [(31, 9), (31, 9), (34, 10), (40, 14), (42, 14), (51, 14),
           (46, 14), (46, 14), (42, 14), (42, 14), (42, 14), (39, 14)]


def load_dump(libfaad):
    here = os.path.dirname(os.path.abspath(__file__))
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, 'dump')
        subprocess.check_call(['gcc', '-I' + libfaad, '-I' + os.path.join(libfaad, '..'), '-include', 'stdint.h',
                               '-o', exe, os.path.join(here, 'aac_dump_hcb.c')])
        out = subprocess.check_output([exe]).decode()
    books = {}
    for line in out.splitlines():
        name, ln, code, a, b, c, d = line.split()
        books.setdefault(name, []).append((int(ln), int(code), int(a), int(b), int(c), int(d)))
    return books


def check_prefix_free(entries, what):
    codes = sorted(((ln, code) for ln, code, *_ in entries))
    # left-align every code to the max length and check intervals do not overlap
    maxlen = max(ln for ln, _ in codes)
    iv = sorted(((code << (maxlen - ln)), ((code + 1) << (maxlen - ln))) for ln, code in codes)
    for (a0, a1), (b0, b1) in zip(iv, iv[1:]):
        assert a1 <= b0, what + ': codes overlap'
    kraft = sum(1.0 / (1 << ln) for ln, _ in codes)
    return kraft


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    books = load_dump(sys.argv[1])

    spec = {}
    for cb, (nv, mod, off) in CB.items():
        ents = books['cb%d' % cb]
        n = mod ** nv
        assert len(ents) == n, (cb, len(ents), n)
        check_prefix_free(ents, 'cb%d' % cb)
        tab = [None] * n
        for ln, code, *vals in ents:
            vals = vals[:nv]
            idx = 0
            for v in vals:
                dig = v + off
                assert 0 <= dig < mod, (cb, vals)
                idx = idx * mod + dig
            assert tab[idx] is None, (cb, idx)
            assert 1 <= ln <= 16 and code < (1 << ln)
            tab[idx] = (ln, code)
        spec[cb] = tab

    sf = books['sf']
    assert len(sf) == 121
    kraft = check_prefix_free(sf, 'sf')
    sftab = [None] * 121
    for ln, code, v, *_ in sf:
        assert sftab[v] is None
        sftab[v] = (ln, code)

    for i, (lc, sc) in enumerate(SR_INDEX):
        assert len(SWB_LONG[lc]) == NUM_SWB_LONG[i] + 1, ('long', i, len(SWB_LONG[lc]))
        assert len(SWB_SHORT[sc]) == NUM_SWB_SHORT[i] + 1, ('short', i, len(SWB_SHORT[sc]))
        assert SWB_LONG[lc][-1] == 1024 and SWB_SHORT[sc][-1] == 128
        for t in (SWB_LONG[lc], SWB_SHORT[sc]):
            assert all(a < b for a, b in zip(t, t[1:]))

    o = []
    p = o.append
    p('// Generated by tools/gen_aac_tables.py - do not edit.')
    p('// AAC spectral / scalefactor Huffman codebooks (indexed by the codeword index of ISO/IEC 14496-3,')
    p('// i.e. the value tuple as a mixed-radix number) and the scalefactor band tables.')
    p('')
    p('#include "aac_tables.h"')
    p('')
    for cb in range(1, 12):
        tab = spec[cb]
        p('static const uint8_t hcb%d_len[%d] = {' % (cb, len(tab)))
        for i in range(0, len(tab), 24):
            p('    ' + ', '.join(str(l) for l, _ in tab[i:i + 24]) + ',')
        p('};')
        p('static const uint16_t hcb%d_code[%d] = {' % (cb, len(tab)))
        for i in range(0, len(tab), 12):
            p('    ' + ', '.join('0x%04x' % c for _, c in tab[i:i + 12]) + ',')
        p('};')
    p('')
    p('const struct aac_hcb aac_hcb[11] = {')
    for cb in range(1, 12):
        nv, mod, off = CB[cb]
        p('    { %d, %d, %d, %d, hcb%d_len, hcb%d_code },' % (nv, mod, off, mod ** nv, cb, cb))
    p('};')
    p('')
    p('const uint8_t aac_sf_len[121] = {')
    for i in range(0, 121, 24):
        p('    ' + ', '.join(str(l) for l, _ in sftab[i:i + 24]) + ',')
    p('};')
    p('const uint32_t aac_sf_code[121] = {')
    for i in range(0, 121, 8):
        p('    ' + ', '.join('0x%05x' % c for _, c in sftab[i:i + 8]) + ',')
    p('};')
    p('')
    p('const int aac_sample_rates[13] = { ' + ', '.join(map(str, SAMPLE_RATES)) + ' };')
    p('const uint8_t aac_num_swb_long[12] = { ' + ', '.join(map(str, NUM_SWB_LONG)) + ' };')
    p('const uint8_t aac_num_swb_short[12] = { ' + ', '.join(map(str, NUM_SWB_SHORT)) + ' };')
    p('const uint8_t aac_tns_max_long[12] = { ' + ', '.join(str(a) for a, _ in TNS_MAX) + ' };')
    p('const uint8_t aac_tns_max_short[12] = { ' + ', '.join(str(b) for _, b in TNS_MAX) + ' };')
    p('')
    p('const uint16_t aac_swb_long[12][52] = {')
    for i, (lc, _) in enumerate(SR_INDEX):
        t = SWB_LONG[lc]
        p('    { ' + ', '.join(map(str, t + [0] * (52 - len(t)))) + ' },')
    p('};')
    p('const uint16_t aac_swb_short[12][16] = {')
    for i, (_, sc) in enumerate(SR_INDEX):
        t = SWB_SHORT[sc]
        p('    { ' + ', '.join(map(str, t + [0] * (16 - len(t)))) + ' },')
    p('};')
    sys.stdout.write('\n'.join(o) + '\n')
    sys.stderr.write('aac tables ok (scalefactor code Kraft sum %.6f)\n' % kraft)


main()
