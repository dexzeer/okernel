# python3 tests/web/wpt/wpt_diff.py <test-rel> [ref-rel]  -> prints the diff bbox, writes build-host/diff.png (test | ref | diff); WBROWSE=... picks the binary
import sys, os, struct, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_wpt as R
R.PORT = int(os.environ.get("SHOW_PORT", "8128"))
R.start_server()
test = sys.argv[1]
p = R.LinkParser()
p.feed(open(os.path.join(R.WPT_DIR, "css", test), encoding="utf-8", errors="replace").read(262144))
ref = sys.argv[2] if len(sys.argv) > 2 else R.resolve_ref(test, (p.match or p.mismatch)[0])
tmp = "build-host/wpt_tmp"
os.makedirs(tmp, exist_ok=True)
ok1, e1 = R.render("css/" + test, tmp + "/dt.ppm")
ok2, e2 = R.render(ref, tmp + "/dr.ppm")
if not (ok1 and ok2):
    print("render failed", e1, e2); sys.exit(1)
w, h, a = R.read_ppm(tmp + "/dt.ppm")
w2, h2, b = R.read_ppm(tmp + "/dr.ppm")
minx, miny, maxx, maxy, n = w, h, -1, -1, 0
d = bytearray(w * h * 3)
for y in range(h):
    for x in range(w):
        i = (y * w + x) * 3
        if a[i:i + 3] != b[i:i + 3]:
            n += 1
            minx = min(minx, x); maxx = max(maxx, x); miny = min(miny, y); maxy = max(maxy, y)
            d[i:i + 3] = b"\xff\x00\x00"
        else:
            g = (a[i] + a[i + 1] + a[i + 2]) // 3
            v = 255 - (255 - g) // 3
            d[i:i + 3] = bytes((v, v, v))
print("differing", n, "bbox", (minx, miny, maxx, maxy) if n else None, "size", w, h)
if n:
    x0, y0 = max(0, minx - 20), max(0, miny - 20)
    x1, y1 = min(w, maxx + 20), min(h, maxy + 20)
else:
    x0, y0, x1, y1 = 0, 0, min(w, 400), min(h, 200)
cw, ch = x1 - x0, y1 - y0
def crop(buf):
    rows = []
    for y in range(y0, y1):
        rows.append(bytes(buf[(y * w + x0) * 3:(y * w + x1) * 3]))
    return rows
panels = [crop(a), crop(b), crop(d)]
out = []
for y in range(ch):
    out.append(b"".join(p[y] + b"\x80\x80\x80" * 2 for p in panels))
W = (cw + 2) * 3
raw = b"".join(b"\x00" + r for r in out)
def chunk(t, dta):
    c = struct.pack(">I", len(dta)) + t + dta
    return c + struct.pack(">I", zlib.crc32(t + dta) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, ch, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
open("build-host/diff.png", "wb").write(png)
