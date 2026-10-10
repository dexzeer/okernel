#!/usr/bin/env python3
"""VP9 test vectors made with a host ffmpeg (test tooling only - nothing from ffmpeg or libvpx is
in the kernel): tests/media/vec/vp9_*.webm, and for each the frames ffmpeg's decoder produces
(vp9_*.yuv, raw planar, 8 bit as bytes, high bit depth as 16-bit little endian) which our
decoder is compared against, sample for sample.

    FFMPEG=/path/to/ffmpeg python3 tests/media/gen_vp9_vectors.py [name ...]
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
VEC = os.path.join(HERE, "vec")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")

def src(kind, w, h, n):
    if kind == "testsrc":
        return f"testsrc2=size={w}x{h}:rate=30,format=yuv420p"
    if kind == "mandel":
        return f"mandelbrot=size={w}x{h}:rate=30:maxiter=60"
    if kind == "life":
        return f"life=size={w}x{h}:rate=30:ratio=0.2:life_color=#ffffff:death_color=#101030,format=yuv420p"
    if kind == "rgb":
        return f"rgbtestsrc=size={w}x{h}:rate=30"
    raise ValueError(kind)

X = ["-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "2", "-threads", "1", "-row-mt", "0"]

# name: (source kind, w, h, frames, pix_fmt, extra encoder args)
CASES = {
    "intra":      ("mandel", 320, 180, 6, "yuv420p", ["-g", "1", "-b:v", "800k"]),
    "intra_hq":   ("testsrc", 320, 180, 4, "yuv420p", ["-g", "1", "-crf", "8", "-b:v", "0"]),
    "inter":      ("testsrc", 320, 180, 40, "yuv420p", ["-b:v", "300k", "-auto-alt-ref", "1", "-lag-in-frames", "16"]),
    "inter_mandel": ("mandel", 320, 180, 40, "yuv420p", ["-b:v", "500k", "-auto-alt-ref", "1", "-lag-in-frames", "16"]),
    "noalt":      ("testsrc", 320, 180, 30, "yuv420p", ["-b:v", "300k", "-auto-alt-ref", "0", "-lag-in-frames", "0"]),
    "life":       ("life", 320, 180, 30, "yuv420p", ["-b:v", "1500k", "-auto-alt-ref", "1", "-lag-in-frames", "10"]),
    "lossless":   ("testsrc", 160, 96, 8, "yuv420p", ["-lossless", "1"]),
    "tiles":      ("testsrc", 640, 360, 12, "yuv420p", ["-b:v", "600k", "-tile-columns", "1", "-tile-rows", "1"]),
    "odd":        ("testsrc", 325, 181, 12, "yuv420p", ["-b:v", "300k"]),
    "tiny":       ("testsrc", 24, 18, 12, "yuv420p", ["-b:v", "50k"]),
    "aq3":        ("testsrc", 320, 180, 30, "yuv420p", ["-b:v", "250k", "-aq-mode", "3", "-lag-in-frames", "0"]),
    "aq1":        ("mandel", 320, 180, 30, "yuv420p", ["-b:v", "400k", "-aq-mode", "1"]),
    "errres":     ("testsrc", 320, 180, 20, "yuv420p", ["-b:v", "300k", "-error-resilient", "1"]),
    "fpar":       ("testsrc", 320, 180, 20, "yuv420p", ["-b:v", "300k", "-frame-parallel", "1"]),
    "yuv444":     ("testsrc", 160, 96, 12, "yuv444p", ["-b:v", "300k", "-profile:v", "1"]),
    "yuv422":     ("testsrc", 160, 96, 12, "yuv422p", ["-b:v", "300k", "-profile:v", "1"]),
    "yuv440":     ("testsrc", 160, 96, 12, "yuv440p", ["-b:v", "300k", "-profile:v", "1"]),
    "p10":        ("testsrc", 160, 96, 12, "yuv420p10le", ["-b:v", "300k", "-profile:v", "2"]),
    "p12":        ("testsrc", 160, 96, 8, "yuv420p12le", ["-b:v", "300k", "-profile:v", "2"]),
    "hq":         ("mandel", 320, 180, 20, "yuv420p", ["-crf", "12", "-b:v", "0", "-auto-alt-ref", "1", "-lag-in-frames", "8"]),
    "lq":         ("mandel", 320, 180, 20, "yuv420p", ["-crf", "58", "-b:v", "0", "-auto-alt-ref", "1", "-lag-in-frames", "8"]),
    "slow":       ("testsrc", 320, 180, 30, "yuv420p", ["-b:v", "300k", "-cpu-used", "0", "-auto-alt-ref", "1", "-lag-in-frames", "25"]),
}

def P(path):
    if FFMPEG.lower().endswith(".exe"):
        return subprocess.check_output(["wslpath", "-w", path]).decode().strip()
    return path

def main():
    os.makedirs(VEC, exist_ok=True)
    only = sys.argv[1:]
    for name, (kind, w, h, n, pix, extra) in CASES.items():
        if only and name not in only:
            continue
        out = os.path.join(VEC, f"vp9_{name}.webm")
        ref = os.path.join(VEC, f"vp9_{name}.yuv")
        cmd = [FFMPEG, "-y", "-v", "error", "-f", "lavfi", "-i", src(kind, w, h, n), "-frames:v", str(n),
               "-pix_fmt", pix] + X + extra + [P(out)]
        subprocess.run(cmd, check=True)
        subprocess.run([FFMPEG, "-y", "-v", "error", "-i", P(out), "-fps_mode", "passthrough", "-f", "rawvideo",
                        "-pix_fmt", pix, P(ref)], check=True)
        print(f"{name:14s} {os.path.getsize(out):8d} bytes webm, {os.path.getsize(ref):9d} bytes yuv")

main()
