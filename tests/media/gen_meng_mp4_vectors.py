#!/usr/bin/env python3
"""MP4 vectors for the media engine test (host ffmpeg, test tooling only):
  meng_h264.mp4 / .yuv        320x180 30 fps High profile with B frames, plus the decoded frames in display order
  meng_h264_frag.mp4          the same video, fragmented (the MediaSource shape)
  meng_aac44.mp4 / .ref       stereo AAC at 44.1 kHz and what ffmpeg decodes+resamples it to (48 kHz s16le stereo)
  meng_aac24_mono.mp4 / .ref  mono AAC at 24 kHz, same
  meng_av.mp4                 H.264 + AAC (48 kHz) together
  meng_aac_frag.mp4           the audio alone, fragmented (MediaSource shape)

    FFMPEG=... python3 tests/media/gen_meng_mp4_vectors.py
"""
import os, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
VEC = os.path.join(HERE, "vec")
FF = os.environ.get("FFMPEG", "ffmpeg")
VIDEO = "testsrc2=size=320x180:rate=30:duration=3"
H264 = ["-c:v", "libx264", "-profile:v", "high", "-x264-params", "keyint=30:bframes=2", "-pix_fmt", "yuv420p"]

def run(*a):
    subprocess.run([FF, "-y", "-v", "error"] + list(a), check=True)

def music(rate, ch):
    if ch == 1:
        return "aevalsrc='0.3*sin(2*PI*330*t)+0.1*sin(2*PI*1760*t)':s=%d:d=3" % rate
    return "aevalsrc='0.3*sin(2*PI*261.6*t)+0.2*sin(2*PI*1320*t)|0.3*sin(2*PI*392*t)+0.1*sin(2*PI*880*t)':s=%d:d=3" % rate

def p(n): return os.path.join(VEC, n)

os.makedirs(VEC, exist_ok=True)
run("-f", "lavfi", "-i", VIDEO, *H264, "-movflags", "+faststart", p("meng_h264.mp4"))
run("-i", p("meng_h264.mp4"), "-f", "rawvideo", "-pix_fmt", "yuv420p", p("meng_h264.yuv"))
run("-f", "lavfi", "-i", VIDEO, *H264, "-movflags", "frag_keyframe+empty_moov+default_base_moof", p("meng_h264_frag.mp4"))
for name, rate, ch in (("meng_aac44", 44100, 2), ("meng_aac24_mono", 24000, 1)):
    run("-f", "lavfi", "-i", music(rate, ch), "-c:a", "aac", "-b:a", "96k", "-aac_pns", "0", p(name + ".mp4"))
    af = "pan=stereo|c0=c0|c1=c0" if ch == 1 else "anull"            # mono -> both channels at full level
    run("-i", p(name + ".mp4"), "-af", af, "-ar", "48000", "-f", "s16le", p(name + ".ref"))
run("-f", "lavfi", "-i", VIDEO, "-f", "lavfi", "-i", music(48000, 2), *H264, "-c:a", "aac", "-b:a", "96k", "-aac_pns", "0",
    "-movflags", "+faststart", p("meng_av.mp4"))
run("-f", "lavfi", "-i", music(48000, 2), "-c:a", "aac", "-b:a", "96k", "-aac_pns", "0",
    "-movflags", "frag_keyframe+empty_moov+default_base_moof", p("meng_aac_frag.mp4"))
print("ok")
