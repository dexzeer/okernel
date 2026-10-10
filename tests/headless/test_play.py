#!/usr/bin/env python3
"""Media playback end to end: `play <url>` downloads a WebM/Opus file from a host HTTP server,
demuxes and decodes it in the kernel and plays it through the HD Audio output; QEMU records what
the guest played. The recording is aligned with the host's own decode of the same file (ffmpeg,
test tooling only) and compared sample by sample: the kernel's decoder must reproduce it.

    FFMPEG=/path/to/ffmpeg python3 tests/headless/test_play.py [file.webm]

Needs the vectors from tests/media/gen_webm_vectors.py (tests/media/vec/webm_audio_only.webm).
"""
import os, re, subprocess, sys, time, wave
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from okvm import OkVM

VEC = os.path.join(HERE, "..", "media", "vec")
name = sys.argv[1] if len(sys.argv) > 1 else "webm_audio_only.webm"
src = os.path.join(VEC, name)
if not os.path.exists(src):
    print(f"  SKIP  {src} missing (run tests/media/gen_webm_vectors.py)")
    sys.exit(0)
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")

WAV = os.path.expanduser("~/okvm/play_test.wav")
if os.path.exists(WAV): os.unlink(WAV)
os.environ["OKVM_AUDIO"] = WAV

# expected samples: for a remuxed Opus vector the RFC reference decoder's output for the same
# packets (minus the Opus pre-skip); otherwise ffmpeg's decode (test tooling only)
def host_path(p):
    if FFMPEG.lower().endswith(".exe"):
        return subprocess.check_output(["wslpath", "-w", p]).decode().strip()
    return p
oracle = name.startswith("webm_remux_")
if oracle:
    base = name[len("webm_remux_"):-len(".webm")]
    chans = int(re.search(r"channels (\d)", open(os.path.join(VEC, f"opus_{base}.info")).read()).group(1))
    ref = np.fromfile(os.path.join(VEC, f"opus_{base}.ref"), dtype="<i2").astype(np.float64)
    ref = ref.reshape(-1, chans)
    if chans == 1: ref = np.repeat(ref, 2, axis=1)
    head = open(os.path.join(VEC, f"opus_{base}.ogg"), "rb").read()
    h = head.index(b"OpusHead")
    pre_skip = head[h + 10] | head[h + 11] << 8
    ref = ref[pre_skip:]
else:
    ref_file = os.path.expanduser("~/okvm/play_ref.pcm")
    subprocess.run([FFMPEG, "-y", "-v", "error", "-i", host_path(src), "-f", "s16le", "-ar", "48000", "-ac", "2",
                    host_path(ref_file)], check=True)
    ref = np.fromfile(ref_file, dtype="<i2").reshape(-1, 2).astype(np.float64)
print(f"  reference: {len(ref) / 48000:.2f} s ({'RFC reference decoder' if oracle else 'ffmpeg'})")

srv = subprocess.Popen([sys.executable, "-m", "http.server", "8000", "--directory", VEC],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
ok = True
vm = OkVM("play")
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(10)
    # keystroke injection can garble the command when the host is loaded (see TESTING.md):
    # check the URL the kernel echoes and type it again if it is wrong
    want = f"[play] fetching http://10.0.2.2:8000/{name}"
    for attempt in range(4):
        vm.type_string(f"play http://10.0.2.2:8000/{name}\n", delay=0.05)
        time.sleep(3)
        if want in vm.serial():
            break
        print("  (command garbled, retrying)")
        vm.type_string("stopplay\n", delay=0.05)
        time.sleep(2)
    done = vm.wait_for("[play] done", timeout=60 + len(ref) / 48000 * 4)
    log = vm.serial()
    for l in re.findall(r"\[play\][^\n]*", log): print("  ", l)
    time.sleep(1.5)
    if not done: print("  FAIL  playback did not finish"); ok = False
finally:
    vm.kill()
    srv.terminate()

time.sleep(1)
if not os.path.exists(WAV) or os.path.getsize(WAV) < 1000:
    print("  FAIL  no recording"); sys.exit(1)
w = wave.open(WAV, "rb")
rate, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
rec = np.frombuffer(w.readframes(n), dtype="<i2").reshape(-1, ch).astype(np.float64)
w.close()
print(f"  recording: {rate} Hz, {ch} ch, {len(rec) / rate:.2f} s")

# locate the start: first sample above the noise floor
nz = np.nonzero(np.abs(rec[:, 0]) > 8)[0]
if len(nz) == 0: print("  FAIL  silent recording"); sys.exit(1)
# alignment: the lag (near the first audible sample) at which the recording matches the start of
# the reference best, by squared error; cross-correlation is ambiguous on periodic signals
seg = ref[:9600, 0]
best = None
for lag_try in range(max(0, nz[0] - 2400), nz[0] + 2400):
    a = rec[lag_try: lag_try + len(seg), 0]
    if len(a) < len(seg): break
    e = float(np.sum((a - seg) ** 2))
    if best is None or e < best[0]: best = (e, lag_try)
lag = best[1]
m = re.search(r"\[play\] done: \d+ packets, (\d+) frames played", log)
played = int(m.group(1)) if m else len(ref)
# the player trims the end padding the container asks for; the reference may keep it
n_cmp = min(len(ref), len(rec) - lag, played)
a = rec[lag: lag + n_cmp, :2]
b = ref[:n_cmp]
err = a - b
snr = 10 * np.log10(np.sum(b ** 2) / max(np.sum(err ** 2), 1e-9))
maxd = np.max(np.abs(err))
print(f"  aligned at {lag} samples; compared {n_cmp / 48000:.2f} s of {played / 48000:.2f} s played; SNR {snr:.1f} dB, max diff {maxd:.0f}")
if n_cmp < played * 0.99: print("  FAIL  recording shorter than the reference"); ok = False
if snr < (80 if oracle else 50): print("  FAIL  decoded audio differs from the reference"); ok = False
if oracle and maxd > 2: print("  FAIL  more than 2 LSB from the reference decoder"); ok = False
print("PLAY PASS" if ok else "PLAY FAIL")
sys.exit(0 if ok else 1)
