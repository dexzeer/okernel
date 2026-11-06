#!/usr/bin/env python3
"""ALSA on the Linux subsystem: Ubuntu's aplay (real alsa-lib, its configuration files, HW_REFINE and all) plays a 1 kHz tone,
and QEMU records what the sound card got into a WAV file, which is checked for that tone. Nothing goes to real speakers.
aplay comes from tools/lx/dosbox_root.sh; without a copy there the test is skipped.
"""
import math, os, re, struct, sys, tempfile, time, wave
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("OKVM_ISO", os.path.join(HERE, "..", "..", "build-host", "lxtest.iso"))
wav_path = os.path.join(tempfile.gettempdir(), "lxsound.wav")
os.environ["OKVM_AUDIO"] = wav_path
os.environ.setdefault("OKVM_AUDIO_CARD", "ac97")
os.environ.setdefault("OKVM_MEM", "2048")
from okvm import OkVM

ROOT = os.path.join(HERE, "..", "..", "build-host", "extra_real")
if not os.path.exists(os.path.join(ROOT, "bin", "aplay")):
    print("RESULT SKIP: no aplay in build-host/extra_real")
    sys.exit(0)

fails = []
def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails.append(what)

def goertzel(s, f, rate):
    k = 2 * math.cos(2 * math.pi * f / rate); a = b = 0.0
    for v in s: a, b = v + k * a - b, a
    return a * a + b * b - k * a * b

vm = OkVM("lxsound")
try:
    check(vm.wait_for("[lx] Linux subsystem ready", timeout=120), "desktop up")
    time.sleep(8)
    n = vm.serial().count("exited, status"); s = len(vm.serial())
    vm.type_string("lx /bin/aplay -q /data/tone.wav\n")
    end = time.time() + 90
    while time.time() < end and vm.serial().count("exited, status") <= n: time.sleep(0.5)
    seg = vm.serial()[s:]
    check(vm.serial().count("exited, status") > n, "aplay ends")
    check(" exited, status 0" in seg, f"with status 0 ({seg.strip()[-200:]!r})" if " exited, status 0" not in seg else "with status 0")
    check("[snd] stream:" in seg, "it set up the stream")
    check(not re.search(r"\[(ISR|panic)\]", vm.serial()), "no kernel faults")
    time.sleep(2)
finally:
    vm.kill()

time.sleep(1)
with wave.open(wav_path, "rb") as w:
    rate, ch, frames = w.getframerate(), w.getnchannels(), w.getnframes()
    raw = w.readframes(frames)
samples = struct.unpack("<%dh" % (len(raw) // 2), raw)[::ch]
os.unlink(wav_path)
loud = [i for i in range(0, len(samples), 480) if max(abs(v) for v in samples[i:i + 480]) > 3000]
secs = len(loud) * 480 / rate
print(f"     {frames} frames recorded at {rate} Hz, {secs:.2f} s of it loud")
check(0.8 < secs < 1.3, f"one second of sound reached the card ({secs:.2f} s)")
if loud:
    mid = samples[loud[len(loud) // 2]:loud[len(loud) // 2] + 4800]
    on, off, off2 = goertzel(mid, 1000, rate), goertzel(mid, 700, rate), goertzel(mid, 2600, rate)
    check(on > 100 * max(off, off2), f"it is the 1 kHz tone ({on:.3g} against {max(off, off2):.3g})")
print("RESULT", "ALLPASS" if not fails else "FAIL: " + "; ".join(fails)[:600])
sys.exit(1 if fails else 0)
