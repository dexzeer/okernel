#!/usr/bin/env python3
"""H.264 test vectors made with a host ffmpeg/libx264 (test tooling only): for each case an Annex B
stream tests/media/vec/h264_<name>.h264 and the reference decode tests/media/vec/h264_<name>.yuv
(ffmpeg's decoder output, display order, yuv420p), which the decoder under test must match exactly.

    FFMPEG=/path/to/ffmpeg python3 tests/media/gen_h264_vectors.py [name ...]
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
VEC = os.path.join(HERE, "vec")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")

SRC = "testsrc2=size=%s:rate=15"
MAND = "mandelbrot=size=%s:rate=15"


def x264(profile, params, extra=()):
    return ["-c:v", "libx264", "-profile:v", profile, "-pix_fmt", "yuv420p", "-x264-params", params, *extra]


# name: (source, frames, size, encoder args)
CASES = {
    # intra only
    "i_cavlc_nodb":    ("testsrc2", 3, "128x96", x264("baseline", "keyint=1:cabac=0:deblock=0,0:no-deblock=1")),
    "i_cavlc":         ("testsrc2", 3, "128x96", x264("baseline", "keyint=1:cabac=0")),
    "i_cabac":         ("testsrc2", 3, "128x96", x264("main", "keyint=1")),
    "i_high8x8":       ("mandelbrot", 3, "160x112", x264("high", "keyint=1:8x8dct=1")),
    "i_high_cavlc":    ("mandelbrot", 3, "160x112", x264("high", "keyint=1:8x8dct=1:cabac=0")),
    # inter
    "p_cavlc":         ("testsrc2", 12, "128x96", x264("baseline", "keyint=30:cabac=0:bframes=0:ref=1")),
    "p_cabac":         ("testsrc2", 12, "128x96", x264("main", "keyint=30:bframes=0:ref=1")),
    "p_refs":          ("testsrc2", 20, "128x96", x264("high", "keyint=60:bframes=0:ref=5")),
    "b_cavlc":         ("testsrc2", 16, "128x96", x264("main", "keyint=60:cabac=0:bframes=3:b-adapt=0")),
    "b_cabac":         ("testsrc2", 16, "128x96", x264("main", "keyint=60:bframes=3:b-adapt=0")),
    "b_pyramid":       ("mandelbrot", 20, "160x112", x264("high", "keyint=60:bframes=4:b-pyramid=normal:ref=4")),
    "b_temporal":      ("testsrc2", 16, "128x96", x264("main", "keyint=60:bframes=3:b-adapt=0:direct=temporal")),
    "weightp":         ("testsrc2", 20, "128x96", x264("main", "keyint=60:bframes=2:weightp=2:ref=3")),
    "weightb":         ("testsrc2", 20, "128x96", x264("main", "keyint=60:bframes=3:weightb=1:b-adapt=0")),
    "slices":          ("testsrc2", 8, "128x96", x264("main", "keyint=30:bframes=2:slices=4")),
    "high_all":        ("mandelbrot", 24, "192x128", x264("high", "keyint=24:bframes=3:ref=4:8x8dct=1:weightp=2:b-pyramid=normal")),
    "odd_crop":        ("testsrc2", 6, "130x98", x264("high", "keyint=30:bframes=2")),
    # more of the toolbox
    "cqm_jvt":         ("mandelbrot", 10, "160x112", x264("high", "keyint=30:bframes=2:cqm=jvt:8x8dct=1")),
    "cqm_flat_cavlc":  ("mandelbrot", 10, "160x112", x264("high", "keyint=30:bframes=2:cqm=flat:cabac=0:8x8dct=1")),
    "constrained":     ("testsrc2", 12, "128x96", x264("main", "keyint=30:bframes=2:constrained-intra=1")),
    "qp_extremes":     ("mandelbrot", 8, "160x112", x264("high", "keyint=30:bframes=2:qp=4")),
    "qp_high":         ("mandelbrot", 8, "160x112", x264("high", "keyint=30:bframes=2:qp=45")),
    "aq_mbtree":       ("mandelbrot", 20, "192x128", x264("high", "keyint=60:bframes=3:aq-mode=2:rc-lookahead=20:crf=24")),
    "me_far":          ("testsrc2", 16, "256x160", x264("high", "keyint=60:bframes=2:me=tesa:merange=64:subme=9:ref=3")),
    "bframes16":       ("testsrc2", 40, "128x96", x264("high", "keyint=100:bframes=16:b-adapt=2:ref=6:b-pyramid=strict")),
    "slices8_cavlc":   ("testsrc2", 10, "192x128", x264("main", "keyint=30:bframes=2:cabac=0:slices=8")),
    "multi_idr":       ("testsrc2", 24, "128x96", x264("high", "keyint=6:bframes=2:ref=3")),
    "no_deblock_p":    ("testsrc2", 12, "128x96", x264("main", "keyint=30:bframes=2:deblock=-1,-1:no-deblock=1")),
    "deblock_strong":  ("testsrc2", 12, "128x96", x264("main", "keyint=30:bframes=2:deblock=6,6")),
    "partitions_all":  ("mandelbrot", 12, "160x112", x264("high", "keyint=30:bframes=2:partitions=all:subme=7")),
    "direct_auto":     ("testsrc2", 20, "128x96", x264("main", "keyint=60:bframes=3:direct=auto:b-adapt=2")),
    "big":             ("mandelbrot", 6, "640x368", x264("high", "keyint=30:bframes=2:ref=3")),
}


def main():
    os.makedirs(VEC, exist_ok=True)
    names = sys.argv[1:] or list(CASES)
    for name in names:
        src, frames, size, args = CASES[name]
        es = os.path.join(VEC, f"h264_{name}.h264")
        yuv = os.path.join(VEC, f"h264_{name}.yuv")
        subprocess.run([FFMPEG, "-y", "-v", "error", "-f", "lavfi", "-i", f"{src}=size={size}:rate=15", "-frames:v", str(frames),
                        *args, "-f", "h264", es], check=True)
        subprocess.run([FFMPEG, "-y", "-v", "error", "-i", es, "-f", "rawvideo", "-pix_fmt", "yuv420p", yuv], check=True)
        print(name, os.path.getsize(es), "bytes,", os.path.getsize(yuv) // (int(size.split("x")[0]) * int(size.split("x")[1]) * 3 // 2), "frames")


main()
