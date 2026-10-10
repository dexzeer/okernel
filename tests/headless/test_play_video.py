#!/usr/bin/env python3
"""Video playback end to end: `play <url>` downloads a WebM with VP9 video and Opus audio from
a host HTTP server, decodes both in the kernel, shows the video in a window and plays the sound,
paced by the audio clock. The test takes a screenshot while it plays and compares the video
window with the frames ffmpeg decodes from the same file (converted to RGB with the kernel's
integer formula); it also checks that all frames were decoded and the audio ran.

    FFMPEG=/path/to/ffmpeg python3 tests/headless/test_play_video.py [file.webm]

Needs the vectors from tests/media/gen_webm_vectors.py (tests/media/vec/webm_av_long.webm).
"""
import os, re, subprocess, sys, time, wave
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from okvm import OkVM

VEC = os.path.join(HERE, "..", "media", "vec")
name = sys.argv[1] if len(sys.argv) > 1 else "webm_av_long.webm"
src = os.path.join(VEC, name)
if not os.path.exists(src):
    print(f"  SKIP  {src} missing (run tests/media/gen_webm_vectors.py)")
    sys.exit(0)
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")
FFPROBE = os.environ.get("FFPROBE", FFMPEG.replace("ffmpeg", "ffprobe"))
WAV = os.path.expanduser("~/okvm/playv_test.wav")
if os.path.exists(WAV): os.unlink(WAV)
os.environ["OKVM_AUDIO"] = WAV

def host_path(p):
    if FFMPEG.lower().endswith(".exe"):
        return subprocess.check_output(["wslpath", "-w", p]).decode().strip()
    return p

# frame size
probe = subprocess.check_output([FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_entries",
                                 "stream=width,height", "-of", "csv=p=0", host_path(src)]).decode().strip()
W, H = map(int, probe.split(","))
ref_file = os.path.expanduser("~/okvm/playv_ref.yuv")
subprocess.run([FFMPEG, "-y", "-v", "error", "-i", host_path(src), "-fps_mode", "passthrough", "-f", "rawvideo",
                "-pix_fmt", "yuv420p", host_path(ref_file)], check=True)
raw = np.fromfile(ref_file, dtype=np.uint8)
fsz = W * H * 3 // 2
nframes = len(raw) // fsz
print(f"  reference: {nframes} frames of {W}x{H}")

def to_rgb(frame):
    """The kernel's conversion: BT.601 limited range for videos shorter than 720 lines."""
    y = frame[:W * H].reshape(H, W).astype(np.int32) - 16
    u = frame[W * H:W * H + (W // 2) * (H // 2)].reshape(H // 2, W // 2).astype(np.int32) - 128
    v = frame[W * H + (W // 2) * (H // 2):].reshape(H // 2, W // 2).astype(np.int32) - 128
    u = np.repeat(np.repeat(u, 2, 0), 2, 1)[:H, :W]
    v = np.repeat(np.repeat(v, 2, 0), 2, 1)[:H, :W]
    base = 298 * y + 128
    r = (base + 409 * v) >> 8
    g = (base - 100 * u - 208 * v) >> 8
    b = (base + 516 * u) >> 8
    return np.clip(np.stack([r, g, b], -1), 0, 255)

refs = [to_rgb(raw[i * fsz:(i + 1) * fsz]) for i in range(nframes)]

srv = subprocess.Popen([sys.executable, "-m", "http.server", "8000", "--directory", VEC],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
ok = True
vm = OkVM("playv")
shot = None
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(10)
    want = f"[play] fetching http://10.0.2.2:8000/{name}"
    for attempt in range(4):
        vm.type_string(f"play http://10.0.2.2:8000/{name}\n", delay=0.05)
        time.sleep(3)
        if want in vm.serial(): break
        print("  (command garbled, retrying)")
        vm.type_string("stopplay\n", delay=0.05)
        time.sleep(2)
    if not vm.wait_for("client at", timeout=60, poll=0.15):
        print("  FAIL  no video window"); ok = False
    else:
        log = vm.serial()
        m = re.search(r"client at (\d+),(\d+)", log)
        cx, cy = int(m.group(1)), int(m.group(2))
        time.sleep(0.8)
        sw, sh, pix = vm.dump()
        full = np.frombuffer(pix, dtype=np.uint8).reshape(sh, sw, 3)
        try:
            from PIL import Image
            Image.fromarray(full).save(os.path.expanduser("~/okvm/playv_shot.png"))
        except Exception:
            pass
        shot = full[cy:cy + H, cx:cx + W].astype(np.int32)
    done = vm.wait_for("[play] video done", timeout=60 + nframes / 30 * 6)
    time.sleep(1.5)
    log = vm.serial()
    for l in re.findall(r"\[play\][^\n]*", log): print("  ", l)
    if not done: print("  FAIL  playback did not finish"); ok = False
finally:
    vm.kill()
    srv.terminate()

if shot is not None and shot.shape[:2] == (H, W):
    errs = [float(np.mean(np.abs(shot - r))) for r in refs]
    best = int(np.argmin(errs))
    print(f"  screenshot matches frame {best} of {nframes}: mean abs diff {errs[best]:.2f} (next best {sorted(errs)[1]:.2f})")
    if errs[best] > 4.0: print("  FAIL  the window does not show a frame of the video"); ok = False
else:
    print("  FAIL  no screenshot of the video window"); ok = False

m = re.search(r"video done: (\d+) frames decoded, (\d+) shown, (\d+) skipped, (\d+) errors", log)
if m:
    dec, shown, skipped, errs_ = map(int, m.groups())
    print(f"  decoded {dec}/{nframes}, shown {shown}, skipped {skipped}, errors {errs_}")
    if dec != nframes or errs_: print("  FAIL  frames missing or decode errors"); ok = False
else:
    print("  FAIL  no video summary"); ok = False

time.sleep(1)
if os.path.exists(WAV) and os.path.getsize(WAV) > 1000:
    w = wave.open(WAV, "rb")
    rec = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    w.close()
    if np.sqrt(np.mean(rec ** 2)) < 100: print("  FAIL  silent audio"); ok = False
    else: print(f"  audio: {len(rec) / 2 / 48000:.1f} s recorded, rms {np.sqrt(np.mean(rec ** 2)):.0f}")
print("PLAYV PASS" if ok else "PLAYV FAIL")
sys.exit(0 if ok else 1)
