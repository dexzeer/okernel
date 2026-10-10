#!/usr/bin/env python3
"""WebM test vectors made with a host ffmpeg (test tooling only): tests/media/vec/webm_*.webm,
and for each the packet list ffprobe reports (webm_*.pkts: stream, pts_ms, size, flags, crc32)
which our demuxer is compared against.

    FFMPEG=/path/to/ffmpeg FFPROBE=/path/to/ffprobe python3 tests/media/gen_webm_vectors.py
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
VEC = os.path.join(HERE, "vec")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("FFPROBE", "ffprobe")

VIDEO = "testsrc2=size=320x180:rate=30:duration=3"
VIDEO_BIG = "testsrc2=size=640x360:rate=30:duration=2"
AUDIO = "sine=frequency=440:sample_rate=48000:duration=3"
MUSIC = ("aevalsrc='0.3*sin(2*PI*261.6*t)+0.2*sin(2*PI*329.6*t)|0.3*sin(2*PI*392*t)+0.1*(random(0)-0.5)':"
         "s=48000:d=3")

VP9 = ["-c:v", "libvpx-vp9", "-b:v", "300k", "-deadline", "realtime", "-cpu-used", "8", "-g", "30"]
OPUS = ["-c:a", "libopus", "-b:a", "96k", "-application", "audio"]

# name: (inputs, codec args, muxer args)
CASES = {
    "av":         ([VIDEO, MUSIC], VP9 + OPUS, []),
    "video_only": ([VIDEO], VP9, []),
    "audio_only": ([MUSIC], OPUS, []),
    "live":       ([VIDEO, MUSIC], VP9 + OPUS, ["-live", "1"]),
    "small_clusters": ([VIDEO, MUSIC], VP9 + OPUS, ["-cluster_time_limit", "400", "-cluster_size_limit", "20000"]),
    "dash_video": ([VIDEO_BIG], VP9 + ["-keyint_min", "30"], ["-dash", "1", "-f", "webm"]),
    "opus_20ms":  ([MUSIC], OPUS + ["-frame_duration", "20"], []),
    "opus_60ms":  ([MUSIC], ["-c:a", "libopus", "-b:a", "64k", "-frame_duration", "60"], []),
}

def P(path):
    if FFMPEG.lower().endswith(".exe"):
        return subprocess.check_output(["wslpath", "-w", path]).decode().strip()
    return path

def main():
    os.makedirs(VEC, exist_ok=True)
    only = sys.argv[1:]
    for name, (inputs, codec, mux) in CASES.items():
        if only and name not in only:
            continue
        out = os.path.join(VEC, f"webm_{name}.webm")
        cmd = [FFMPEG, "-y", "-v", "error"]
        for i in inputs:
            cmd += ["-f", "lavfi", "-i", i]
        cmd += codec + mux + [P(out)]
        subprocess.run(cmd, check=True)
        probe = subprocess.check_output(
            [FFPROBE, "-v", "error", "-show_packets", "-show_data_hash", "CRC32", "-of", "csv=p=0",
             "-show_entries", "packet=stream_index,pts,duration,size,flags,data_hash", P(out)]).decode()
        with open(os.path.join(VEC, f"webm_{name}.pkts"), "w") as f:
            f.write(probe.replace("\r", ""))
        print(f"{name:16s} {os.path.getsize(out):8d} bytes  {len(probe.splitlines()):4d} packets")

main()
