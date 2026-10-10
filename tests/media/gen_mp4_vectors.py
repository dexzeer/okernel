#!/usr/bin/env python3
"""MP4 test vectors made with a host ffmpeg (test tooling only): tests/media/vec/mp4_<name>.mp4 and the
packet list ffprobe reports for each (mp4_<name>.pkts: stream, pts_time, dts_time, duration_time, size,
flags, crc32) which the demuxer is compared against.

    FFMPEG=... FFPROBE=... python3 tests/media/gen_mp4_vectors.py [name ...]
"""
import os, subprocess, sys, glob, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
VEC = os.path.join(HERE, "vec")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("FFPROBE", "ffprobe")

VIDEO = "testsrc2=size=320x180:rate=30:duration=3"
MUSIC = ("aevalsrc='0.3*sin(2*PI*261.6*t)+0.2*sin(2*PI*329.6*t)|0.3*sin(2*PI*392*t)+0.1*(random(0)-0.5)':s=48000:d=3")
H264 = ["-c:v", "libx264", "-profile:v", "high", "-x264-params", "keyint=30:bframes=2", "-pix_fmt", "yuv420p"]
H264_NOB = ["-c:v", "libx264", "-profile:v", "baseline", "-x264-params", "keyint=30", "-pix_fmt", "yuv420p"]
AAC = ["-c:a", "aac", "-b:a", "96k"]

# name: (inputs, codec args, muxer args)
CASES = {
    "av_faststart":   ([VIDEO, MUSIC], H264 + AAC, ["-movflags", "+faststart"]),
    "av_moov_last":   ([VIDEO, MUSIC], H264 + AAC, []),
    "av_nob":         ([VIDEO, MUSIC], H264_NOB + AAC, ["-movflags", "+faststart"]),
    "video_only":     ([VIDEO], H264, ["-movflags", "+faststart"]),
    "audio_only":     ([MUSIC], AAC, []),
    "frag":           ([VIDEO, MUSIC], H264 + AAC, ["-movflags", "frag_keyframe+empty_moov+default_base_moof"]),
    "frag_legacy":    ([VIDEO, MUSIC], H264 + AAC, ["-movflags", "frag_keyframe+empty_moov"]),
    "frag_video":     ([VIDEO], H264, ["-movflags", "frag_keyframe+empty_moov+default_base_moof+separate_moof"]),
    "vp9":            ([VIDEO], ["-c:v", "libvpx-vp9", "-b:v", "300k", "-deadline", "realtime", "-cpu-used", "8", "-g", "30"], ["-movflags", "+faststart"]),
    "big_chunks":     ([VIDEO, MUSIC], H264 + AAC, ["-movflags", "+faststart", "-frag_duration", "0", "-max_interleave_delta", "0"]),
}


def probe(path, out):
    txt = subprocess.check_output(
        [FFPROBE, "-v", "error", "-show_packets", "-show_data_hash", "CRC32", "-of", "csv=p=0",
         "-show_entries", "packet=stream_index,pts_time,dts_time,duration_time,size,flags,data_hash", path]).decode()
    with open(out, "w") as f:
        f.write(txt.replace("\r", ""))
    return len(txt.splitlines())


def main():
    os.makedirs(VEC, exist_ok=True)
    only = sys.argv[1:]
    for name, (inputs, codec, mux) in CASES.items():
        if only and name not in only:
            continue
        out = os.path.join(VEC, f"mp4_{name}.mp4")
        cmd = [FFMPEG, "-y", "-v", "error"]
        for i in inputs:
            cmd += ["-f", "lavfi", "-i", i]
        cmd += codec + mux + [out]
        subprocess.run(cmd, check=True)
        n = probe(out, os.path.join(VEC, f"mp4_{name}.pkts"))
        print(f"{name:16s} {os.path.getsize(out):8d} bytes  {n:4d} packets")
    # DASH-style: an init segment followed by media segments, concatenated (what MediaSource appends)
    if not only or "dash" in only:
        d = os.path.join(VEC, "dash_tmp")
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        subprocess.run([FFMPEG, "-y", "-v", "error", "-f", "lavfi", "-i", VIDEO, "-f", "lavfi", "-i", MUSIC] + H264 + AAC +
                       ["-f", "dash", "-seg_duration", "1", "-use_template", "1", "-use_timeline", "1", "out.mpd"], check=True, cwd=d)   # (the dash muxer writes segments next to the cwd on Windows paths)
        for rep in ("0", "1"):
            out = os.path.join(VEC, f"mp4_dash{rep}.mp4")
            with open(out, "wb") as o:
                o.write(open(os.path.join(d, f"init-stream{rep}.m4s"), "rb").read())
                for seg in sorted(glob.glob(os.path.join(d, f"chunk-stream{rep}-*.m4s"))):
                    o.write(open(seg, "rb").read())
            n = probe(out, os.path.join(VEC, f"mp4_dash{rep}.pkts"))
            print(f"{'dash' + rep:16s} {os.path.getsize(out):8d} bytes  {n:4d} packets")
        shutil.rmtree(d, ignore_errors=True)


main()
