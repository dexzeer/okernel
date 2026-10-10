#!/usr/bin/env python3
"""AAC test vectors made with a host ffmpeg (test tooling only): tests/media/vec/aac_<name>.aac (ADTS) and the
float PCM ffmpeg decodes it to (aac_<name>.f32, native rate and channel count) which the decoder is compared with.

    FFMPEG=... python3 tests/media/gen_aac_vectors.py [name ...]
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
VEC = os.path.join(HERE, "vec")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")

MUSIC = "aevalsrc='0.3*sin(2*PI*261.6*t)+0.2*sin(2*PI*329.6*t)+0.08*sin(2*PI*3520*t)|0.3*sin(2*PI*392*t)+0.1*(random(0)-0.5)':s=%d:d=3"
MONO = "aevalsrc='0.25*sin(2*PI*(220+110*sin(2*PI*0.7*t))*t)+0.1*(random(0)-0.5)':s=%d:d=3"
# clicks and bursts: transients force EIGHT_SHORT windows (and start/stop windows around them)
TRANS = "aevalsrc='if(lt(mod(t,0.37),0.012),(random(0)-0.5)*1.6,0.2*sin(2*PI*330*t))|if(lt(mod(t,0.53),0.008),(random(1)-0.5)*1.4,0.2*sin(2*PI*495*t))':s=%d:d=4"
# near-mono and phase-inverted content: M/S and intensity stereo
CORR = "aevalsrc='0.3*sin(2*PI*440*t)+0.04*(random(0)-0.5)|0.3*sin(2*PI*440*t)+0.04*(random(1)-0.5)':s=%d:d=3"
ANTI = "aevalsrc='0.3*sin(2*PI*440*t)+0.2*sin(2*PI*1760*t)|-0.3*sin(2*PI*440*t)-0.2*sin(2*PI*1760*t)':s=%d:d=3"
LOUD = "aevalsrc='0.95*sin(2*PI*180*t)+0.5*(random(0)-0.5)|0.9*sin(2*PI*2000*t)':s=%d:d=3"
NOISE = "anoisesrc=color=white:amplitude=0.5:sample_rate=%d:duration=3"

# name: (source, rate, extra encoder args)
CASES = {
    "music44":     (MUSIC, 44100, ["-b:a", "128k"]),
    "music48":     (MUSIC, 48000, ["-b:a", "96k"]),
    "music32":     (MUSIC, 32000, ["-b:a", "64k"]),
    "music24":     (MUSIC, 24000, ["-b:a", "48k"]),
    "music22":     (MUSIC, 22050, ["-b:a", "48k"]),
    "music16":     (MUSIC, 16000, ["-b:a", "32k"]),
    "music11":     (MUSIC, 11025, ["-b:a", "24k"]),
    "music8":      (MUSIC, 8000, ["-b:a", "16k"]),
    "music96":     (MUSIC, 96000, ["-b:a", "256k"]),
    "music64":     (MUSIC, 64000, ["-b:a", "192k"]),
    "mono44":      (MONO, 44100, ["-b:a", "64k"]),
    "mono16":      (MONO, 16000, ["-b:a", "24k"]),
    "trans44":     (TRANS, 44100, ["-b:a", "128k"]),
    "trans48_fast": (TRANS, 48000, ["-b:a", "96k", "-aac_coder", "fast"]),
    "trans22":     (TRANS, 22050, ["-b:a", "64k"]),
    "trans8":      (TRANS, 8000, ["-b:a", "24k"]),
    "corr_ms":     (CORR, 44100, ["-b:a", "96k", "-aac_ms", "1"]),
    "corr_is":     (CORR, 44100, ["-b:a", "32k", "-aac_is", "1"]),
    "anti_ms":     (ANTI, 44100, ["-b:a", "96k", "-aac_ms", "1"]),
    "anti_is":     (ANTI, 44100, ["-b:a", "24k", "-aac_is", "1"]),
    "tns":         (TRANS, 44100, ["-b:a", "96k", "-aac_tns", "1"]),
    "notns":       (TRANS, 44100, ["-b:a", "96k", "-aac_tns", "0"]),
    "loud":        (LOUD, 44100, ["-b:a", "256k"]),
    "hibr":        (NOISE, 44100, ["-b:a", "320k"]),
    "lobr":        (MUSIC, 44100, ["-b:a", "16k"]),
    "pns":         (NOISE, 44100, ["-b:a", "48k", "-aac_pns", "1"]),
    "pns_music":   (MUSIC, 44100, ["-b:a", "32k", "-aac_pns", "1"]),
}


def main():
    os.makedirs(VEC, exist_ok=True)
    only = sys.argv[1:]
    for name, (src, rate, extra) in CASES.items():
        if only and name not in only:
            continue
        aac = os.path.join(VEC, f"aac_{name}.aac")
        raw = os.path.join(VEC, f"aac_{name}.f32")
        s = src % rate if "%d" in src else src
        if not name.startswith("pns"):
            extra = extra + ["-aac_pns", "0"]       # PNS noise is not normative: only the pns cases use it
        subprocess.run([FFMPEG, "-y", "-v", "error", "-f", "lavfi", "-i", s, "-c:a", "aac"] + extra + ["-f", "adts", aac], check=True)
        subprocess.run([FFMPEG, "-y", "-v", "error", "-i", aac, "-f", "f32le", "-acodec", "pcm_f32le", raw], check=True)
        print(f"{name:14s} {os.path.getsize(aac):8d} bytes aac  {os.path.getsize(raw):9d} bytes pcm")


main()
