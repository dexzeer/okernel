#!/usr/bin/env python3
"""okai <video> / MediaSource test (offline, local fixture server on :8002).

  1. tests/web/js/media.html: <video src> (progressive WebM, Opus + VP9) plays to the end, fires
     the event sequence, the page reports frames/size; mid-playback screenshots show the moving
     test pattern inside the page (not blank, changes between shots)
  2. tests/web/js/mse.html: MediaSource with separate audio/video SourceBuffers appended in
     16 KB pieces, a seek back mid-playback, end of stream
PASS requires every step and no faults / allocator errors / realm aborts.
Needs the vectors from tests/media/gen_webm_vectors.py.
"""
import sys, os, time, re, subprocess, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VEC = os.path.join(ROOT, "tests", "media", "vec")
WWW = os.path.join(OUTDIR, "www-media")
shutil.rmtree(WWW, ignore_errors=True)
os.makedirs(os.path.join(WWW, "web", "js"))
os.makedirs(os.path.join(WWW, "media", "vec"))
for p in ("media.html", "mse.html", "mse_offset.html", "mse_changetype.html", "adswap.html"):
    shutil.copy(os.path.join(ROOT, "tests", "web", "js", p), os.path.join(WWW, "web", "js", p))
# the same page on the 10 s clip: long enough to take screenshots while it plays
html = open(os.path.join(ROOT, "tests", "web", "js", "media.html")).read().replace("webm_av.webm", "webm_av_long.webm")
open(os.path.join(WWW, "web", "js", "media_long.html"), "w").write(html)
for v in ("webm_av.webm", "webm_av_long.webm", "webm_video_only.webm", "webm_audio_only.webm", "meng_av.mp4", "meng_h264_frag.mp4", "meng_aac_frag.mp4"):
    if not os.path.exists(os.path.join(VEC, v)):
        print(f"SKIP  {v} missing (run tests/media/gen_webm_vectors.py)")
        sys.exit(0)
    shutil.copy(os.path.join(VEC, v), os.path.join(WWW, "media", "vec", v))

import numpy as np
srv = subprocess.Popen([sys.executable, "-m", "http.server", "8002", "--directory", WWW],
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
time.sleep(1)
results = []


def step(name, ok):
    results.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name, flush=True)


def region(vm, ox, oy, w=320, h=180):
    sw, sh, pix = vm.dump()
    full = np.frombuffer(pix, dtype=np.uint8).reshape(sh, sw, 3)
    return full[oy:oy + h, ox:ox + w].astype(np.int32)


tag = sys.argv[1] if len(sys.argv) > 1 else "okaimedia"
vm = OkVM(tag)
try:
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(10)
    vm.type_string("cpu slow 1\n", delay=0.05)    # QEMU/TCG benchmarks as a slow CPU, which blocks muted autoplay: this test wants the desktop policy
    time.sleep(1)
    base = "http://10.0.2.2:8002/web/js/"
    want = "okai " + base + "media_long.html"
    for attempt in range(4):
        vm.type_string(want + "\n", delay=0.05)
        time.sleep(4)
        if "[okai] fetch " + base + "media_long.html" in vm.serial():
            break
        print("  (command garbled, retrying)")
        vm.type_string("\n", delay=0.05)
        time.sleep(2)
    step("event sequence reaches loadedmetadata", vm.wait_for("EV loadedmetadata", timeout=90))
    step("playing starts", vm.wait_for("EV playing", timeout=60))
    log = vm.serial()
    m = re.search(r"render tab=\d+ \d+x\d+ doc_h=\d+ scroll=\d+ in \d+ms \(page origin (\d+),(\d+)\)", log)
    if m: ox, oy = int(m.group(1)), int(m.group(2))
    else:
        m = re.search(r"\(page origin (\d+),(\d+)\)", log)
        ox, oy = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
    time.sleep(0.4)
    a = region(vm, ox, oy)
    time.sleep(0.7)
    b = region(vm, ox, oy)
    try:
        from PIL import Image
        Image.fromarray(np.concatenate([a, b], axis=1).astype(np.uint8)).save(os.path.expanduser("~/okvm/okaimedia_shots.png"))
    except Exception:
        pass
    colors = len({tuple(c) for c in a[::6, ::6].reshape(-1, 3)})
    step(f"video frame is on screen ({colors} colours in the 320x180 box)", colors >= 6)
    step("the picture moves between two screenshots (mean abs diff %.2f)" % float(np.mean(np.abs(a - b))),
         float(np.mean(np.abs(a - b))) > 0.05)
    step("ended", vm.wait_for("EV ended", timeout=90))
    ok = vm.wait_for("size=320x180 frames=300 ", timeout=20)
    step("page sees 320x180 / 300 decoded frames", ok)
    for l in re.findall(r"\[js\][^\n]*", vm.serial()):
        print("   ", l)

    # 2. MediaSource
    n0 = vm.serial().count("RESULT ")
    time.sleep(1)
    vm.type_string("g")
    time.sleep(0.5)
    vm.type_string(base + "mse.html\n", delay=0.05)
    step("MediaSource: sourceopen", vm.wait_for("sourceopen readyState=open", timeout=90))
    step("MediaSource: playing + seek back", vm.wait_for("seeking back to 0.5", timeout=90))
    step("MediaSource: seeked", vm.wait_for("EV seeked", timeout=30))
    step("MediaSource: ended", vm.wait_for("seeked=true", timeout=90) and vm.wait_for("RESULT dur=3.00 size=320x180 frames=", timeout=5))
    log = vm.serial()
    for l in re.findall(r"\[js\][^\n]*", log)[-20:]:
        print("   ", l)

    # 3. what an ad -> content hand-over exercises: timestampOffset / append windows / per-buffer remove / endOfStream reopen,
    #    changeType between containers, and one <video> element switching sources (progressive MP4 -> MediaSource -> WebM file)
    for page, want in (("mse_offset.html", "RESULT t=9.00 dur=9.00 frames="), ("mse_changetype.html", "RESULT t=6.00 frames=90 size=320x180"), ("adswap.html", "RESULT ALLSTAGES")):
        time.sleep(1)
        vm.type_string("g")
        time.sleep(0.5)
        vm.type_string(base + page + "\n", delay=0.05)
        step(page + " in the kernel", vm.wait_for(want, timeout=120))
    log = vm.serial()
    bad = [l for l in log.splitlines()
           if "kmalloc FAIL" in l or "bad or double free" in l or "PAGE FAULT" in l.upper() or
           "abort()" in l or ("EXCEPTION" in l.upper() and "#" in l)]
    step("no faults / allocator errors / realm aborts", not bad)
    if bad: print("   ", bad[:5])
    errs = [l for l in log.splitlines() if l.startswith("[js] error")]
    print("js errors:", len(errs), errs[:5])
finally:
    vm.kill()
    srv.terminate()
print("RESULT:", "PASS" if results and all(r for _, r in results) else "FAIL")
sys.exit(0 if results and all(r for _, r in results) else 1)
