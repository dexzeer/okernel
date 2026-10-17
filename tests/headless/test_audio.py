#!/usr/bin/env python3
"""Audio output end to end: QEMU's Intel HD Audio controller + codec records
whatever the guest plays to a WAV file (-audiodev wav). The kernel's `beep` command
plays a tone through the HD Audio driver; this test checks the recording: format,
dominant frequency (FFT), level, duration, and that the tone has no dropouts.

    python3 tests/headless/test_audio.py
"""
import sys, os, time, re, wave
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

WAV = os.path.expanduser("~/okvm/audio_test.wav")
if os.path.exists(WAV): os.unlink(WAV)
os.environ["OKVM_AUDIO"] = WAV

def analyze(path):
    w = wave.open(path, "rb")
    ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
    w.close()
    x = np.frombuffer(raw, dtype="<i2").reshape(-1, ch).astype(np.float64)
    return rate, ch, sw, x

ok = True
vm = OkVM("audio")
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(10)
    log = vm.serial()
    if "[hda] audio output running" not in log and "[hda] AC'97 audio output running" not in log:
        print("  FAIL  HD Audio did not start:", re.findall(r"\[hda\][^\n]*", log)[:6]); ok = False
    else:
        print("  driver:", re.findall(r"\[hda\] (?:codec|output path)[^\n]*", log))
    for hz in (440, 1000):
        vm.type_string(f"beep {hz} 1000\n", delay=0.03)
        vm.wait_for(r"beep done", timeout=15)
        time.sleep(1.5)
    vm.type_string("audio\n", delay=0.03)
    time.sleep(2)
finally:
    vm.kill()

time.sleep(1)
if not os.path.exists(WAV) or os.path.getsize(WAV) < 1000:
    print("  FAIL  no recording"); sys.exit(1)
rate, ch, sw, x = analyze(WAV)
print(f"  recording: {rate} Hz, {ch} ch, {sw * 8} bit, {len(x) / rate:.2f} s")
if (ch, sw) != (2, 2): print("  FAIL  format"); ok = False
mono = x[:, 0]
level = np.abs(mono) > 500
idx = np.nonzero(level)[0]
if len(idx) == 0:
    print("  FAIL  silence"); sys.exit(1)

# find the two tone bursts (gaps of silence > 0.3 s separate them)
bursts = []
start = idx[0]; prev = idx[0]
for i in idx[1:]:
    if i - prev > rate * 0.3:
        bursts.append((start, prev)); start = i
    prev = i
bursts.append((start, prev))
print(f"  tone bursts: {[(round(a / rate, 2), round((b - a) / rate, 2)) for a, b in bursts]}")
if len(bursts) != 2: print("  FAIL  expected 2 bursts"); ok = False
for (a, b), want in zip(bursts, (440, 1000)):
    seg = mono[a:b]
    dur = len(seg) / rate
    # skip 50 ms at each edge, FFT the middle
    mid = seg[int(rate * 0.05): -int(rate * 0.05)]
    spec = np.abs(np.fft.rfft(mid * np.hanning(len(mid))))
    peak = np.argmax(spec) * rate / len(mid)
    rms = np.sqrt(np.mean(mid ** 2))
    # dropouts: zero runs inside the burst
    z = (np.abs(mid) < 20).astype(np.int8)
    # a sine crosses ~0 every half period; a dropout is a long run
    runs = 0; cur = 0; longest = 0
    for v in z:
        cur = cur + 1 if v else 0
        longest = max(longest, cur)
    good = abs(peak - want) < 8 and 0.9 < dur < 1.15 and 1500 < rms < 12000 and longest < rate * 0.004
    print(f"  {want} Hz: peak {peak:.1f} Hz, {dur:.2f} s, rms {rms:.0f}, longest near-zero run {longest / rate * 1000:.1f} ms -> {'PASS' if good else 'FAIL'}")
    ok = ok and good
print("AUDIO", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
