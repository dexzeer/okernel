#!/usr/bin/env python3
"""Generate Opus test vectors with a host ffmpeg (test tooling only - nothing from
ffmpeg or libopus is in the kernel): tests/media/vec/opus_*.ogg, then for each one

  opus_*.bit   the packets in opus_demo's framing (len, final range, payload)
  opus_*.ref   what the RFC 6716 reference decoder produces (48 kHz s16le, same
               channel count as the stream) - the oracle our decoder is compared to
  opus_*.info  one line: packet count and the TOC configurations used

    FFMPEG=/path/to/ffmpeg ORACLE=~/opus_oracle python3 tests/media/gen_opus_vectors.py
"""
import os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
VEC = os.path.join(HERE, "vec")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
ORACLE = os.path.expanduser(os.environ.get("ORACLE", "~/opus_oracle"))

# a harmonic "voice" with vibrato + syllable-rate amplitude, plus a little noise;
# the second input is a decaying-click "percussion" for the music-like modes
VOICE = ("aevalsrc='0.30*sin(2*PI*(150+40*sin(2*PI*0.6*t))*t)*(0.6+0.4*sin(2*PI*3*t))"
         "+0.15*sin(2*PI*2*(150+40*sin(2*PI*0.6*t))*t)*(0.6+0.4*sin(2*PI*3*t))"
         "+0.08*sin(2*PI*5*(150+40*sin(2*PI*0.6*t))*t)':s=48000:d=4")
PERC = ("aevalsrc='0.5*sin(2*PI*220*t)*exp(-8*mod(t,0.25))+0.3*sin(2*PI*1760*t)*exp(-30*mod(t,0.5))"
        "+0.2*(random(0)-0.5)*exp(-20*mod(t+0.1,0.5))':s=48000:d=4")

# decorrelated stereo "music": chords on both channels, different noise floors, a bright
# decaying click train on the right only
MUSIC = ("aevalsrc='0.25*sin(2*PI*261.6*t)+0.2*sin(2*PI*329.6*t)+0.2*sin(2*PI*392*t)"
         "+0.12*(random(0)-0.5)*(0.5+0.5*sin(2*PI*2*t))"
         "|0.25*sin(2*PI*220*t)+0.2*sin(2*PI*277*t)+0.15*sin(2*PI*440*t)"
         "+0.12*(random(1)-0.5)*(0.5+0.5*sin(2*PI*1.5*t))+0.25*sin(2*PI*3000*t)*exp(-6*mod(t,0.4))':s=48000:d=4")
NOISE = "aevalsrc='0.4*(random(0)-0.5)*(0.3+0.7*sin(2*PI*0.7*t)*sin(2*PI*0.7*t))':s=48000:d=4"

# name: (source, extra ffmpeg args)
CASES = {
    "celt_fb_st":     (PERC,  "-ac 2 -application audio -b:a 128k"),
    "celt_fb_mono":   (VOICE, "-ac 1 -application audio -b:a 64k"),
    "celt_nb":        (PERC,  "-ac 1 -application audio -cutoff 4000 -b:a 24k"),
    "celt_wb":        (PERC,  "-ac 1 -application audio -cutoff 8000 -b:a 32k"),
    "celt_swb":       (PERC,  "-ac 2 -application audio -cutoff 12000 -b:a 64k"),
    "celt_2_5ms":     (PERC,  "-ac 2 -application lowdelay -frame_duration 2.5 -b:a 96k"),
    "celt_5ms":       (PERC,  "-ac 2 -application lowdelay -frame_duration 5 -b:a 96k"),
    "celt_10ms":      (PERC,  "-ac 2 -application audio -frame_duration 10 -b:a 96k"),
    "celt_cbr":       (PERC,  "-ac 2 -application audio -vbr off -b:a 96k"),
    "celt_ld_st_12k": (MUSIC, "-ac 2 -application lowdelay -b:a 12k"),
    "celt_ld_st_24k": (MUSIC, "-ac 2 -application lowdelay -b:a 24k"),
    "celt_ld_st_48k": (MUSIC, "-ac 2 -application lowdelay -b:a 48k"),
    "celt_ld_st_256k": (MUSIC, "-ac 2 -application lowdelay -b:a 256k"),
    "celt_ld_mono_8k": (NOISE, "-ac 1 -application lowdelay -b:a 8k"),
    "celt_ld_mono_64k": (NOISE, "-ac 1 -application lowdelay -b:a 64k"),
    "celt_ld_st_10ms_24k": (MUSIC, "-ac 2 -application lowdelay -frame_duration 10 -b:a 24k"),
    "celt_ld_st_5ms_32k": (MUSIC, "-ac 2 -application lowdelay -frame_duration 5 -b:a 32k"),
    "silk_nb":        (VOICE, "-ac 1 -application voip -cutoff 4000 -b:a 10k"),
    "silk_mb":        (VOICE, "-ac 1 -application voip -cutoff 6000 -b:a 14k"),
    "silk_wb":        (VOICE, "-ac 1 -application voip -cutoff 8000 -b:a 20k"),
    "silk_wb_10ms":   (VOICE, "-ac 1 -application voip -cutoff 8000 -b:a 24k -frame_duration 10"),
    "silk_wb_40ms":   (VOICE, "-ac 1 -application voip -cutoff 8000 -b:a 20k -frame_duration 40"),
    "silk_wb_60ms":   (VOICE, "-ac 1 -application voip -cutoff 8000 -b:a 20k -frame_duration 60"),
    "silk_st":        (VOICE, "-ac 2 -application voip -cutoff 8000 -b:a 32k"),
    "hybrid_swb":     (VOICE, "-ac 1 -application voip -cutoff 12000 -b:a 36k"),
    "hybrid_fb":      (VOICE, "-ac 1 -application voip -cutoff 20000 -b:a 40k"),
    "hybrid_fb_st":   (VOICE, "-ac 2 -application voip -cutoff 20000 -b:a 56k"),
    "multi_120ms":    (VOICE, "-ac 1 -application voip -cutoff 8000 -b:a 20k -frame_duration 120"),
}

def P(path):
    """A path the ffmpeg binary understands (a Windows .exe run from WSL needs C:\\ style)."""
    if FFMPEG.lower().endswith(".exe"):
        return subprocess.check_output(["wslpath", "-w", path]).decode().strip()
    return path

def ogg_packets(path):
    """Opus packets of an Ogg Opus file (skips the two header packets)."""
    data = open(path, "rb").read()
    pos, packets, cur = 0, [], b""
    while pos + 27 <= len(data):
        assert data[pos:pos + 4] == b"OggS", "bad ogg"
        nseg = data[pos + 26]
        lacing = data[pos + 27:pos + 27 + nseg]
        pos += 27 + nseg
        for l in lacing:
            cur += data[pos:pos + l]
            pos += l
            if l < 255:
                packets.append(cur); cur = b""
    return packets[2:]

def main():
    os.makedirs(VEC, exist_ok=True)
    only = sys.argv[1:]
    for name, (src, args) in CASES.items():
        if only and name not in only:
            continue
        ogg = os.path.join(VEC, f"opus_{name}.ogg")
        cmd = [FFMPEG, "-y", "-v", "error", "-f", "lavfi", "-i", src] + args.split() + ["-c:a", "libopus", P(ogg)]
        subprocess.run(cmd, check=True)
        pk = ogg_packets(ogg)
        chans = 2 if "-ac 2" in args else 1
        base = os.path.join(VEC, f"opus_{name}")
        with open(base + ".bit", "wb") as f:
            for p in pk:
                f.write(struct.pack(">II", len(p), 0))    # final range unknown to us: 0 (the demo only checks if nonzero)
                f.write(p)
        # oracle decode: opus_demo -d <rate> <channels> in.bit out.pcm
        subprocess.run([os.path.join(ORACLE, "opus_demo"), "-d", "48000", str(chans), base + ".bit", base + ".ref"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cfgs = sorted({p[0] >> 3 for p in pk})
        modes = {"silk" if c < 12 else "hybrid" if c < 16 else "celt" for c in cfgs}
        with open(base + ".info", "w") as f:
            f.write(f"{len(pk)} packets, channels {chans}, configs {cfgs}, modes {sorted(modes)}\n")
        print(f"{name:14s} {len(pk):4d} packets  configs {cfgs}  {sorted(modes)}")

main()
