#!/usr/bin/env python3
# WPT CSS reftest runner for the okernel web engine.
#
# Renders each reftest (test page + its match/mismatch references) with
# build-host/wbrowse at 800x600 and diffs the viewports pixel by pixel.
# Match refs must render identically (within a fuzzy tolerance when the
# test declares one via <meta name=fuzzy>); mismatch refs must not.
#
# Run inside WSL from the repo root (WPT tree expected at ~/wpt, clone with:
#   git clone --depth 1 --filter=blob:none --sparse \
#     https://github.com/web-platform-tests/wpt.git ~/wpt
#   cd ~/wpt && git sparse-checkout set css fonts tools
# ):
#   python3 tests/web/wpt/run_wpt.py CSS2 css-box css-display selectors
#   python3 tests/web/wpt/run_wpt.py --list css-flexbox        # just count
#   python3 tests/web/wpt/run_wpt.py -j4 -o build-host/wpt_out css-sizing
#
# Results land in <out>/summary.txt and <out>/results.json.
import argparse, functools, html.parser, http.server, json, os, posixpath, re
import subprocess, sys, threading, urllib.parse
from multiprocessing import Pool

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
WBROWSE = os.environ.get("WBROWSE") or os.path.join(REPO, "build-host", "wbrowse")
WPT_DIR = os.environ.get("WPT_DIR", os.path.expanduser("~/wpt"))
PORT = int(os.environ.get("WPT_PORT", "8125"))
VW, VH = 800, 600
XHT_TESTS = os.environ.get("WPT_XHT") == "2"          # WPT_XHT=2: tests written as .xht / .xhtml / .htm are discovered too
XHT = bool(os.environ.get("WPT_XHT"))     # WPT_XHT=1: references written as .xht / .xhtml / .htm are rendered too (the default skips them)
RENDER_SECONDS = "2.5"


class LinkParser(html.parser.HTMLParser):
    # <link rel=match|mismatch href=...> plus <meta name=fuzzy content=...>
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.match = []      # hrefs of rel=match (and rel=match-*)
        self.mismatch = []
        self.fuzzy = None

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "link":
            rel = (a.get("rel") or "").lower()
            href = a.get("href")
            if not href:
                return
            if rel == "match" or rel.startswith("match-"):
                self.match.append(href)
            elif rel == "mismatch" or rel.startswith("mismatch-"):
                self.mismatch.append(href)
        elif tag == "meta" and (a.get("name") or "").lower() == "fuzzy":
            self.fuzzy = a.get("content")


FUZZ_RE = re.compile(r"maxDifference\s*=\s*(\d+)-(\d+)\s*;\s*total\s*=\s*(\d+)-(\d+)")


def discover(area):
    """Yield (test_path, [match_hrefs], [mismatch_hrefs], fuzzy_meta) for one area."""
    root = os.path.join(WPT_DIR, "css", area)
    walker = [(os.path.dirname(root), [], [os.path.basename(root)])] if os.path.isfile(root) else os.walk(root)
    for dirpath, _dirs, files in walker:
        for fn in files:
            if not (fn.endswith(".html") or (XHT_TESTS and fn.endswith((".xht", ".xhtml", ".htm")))):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    src = f.read(262144)
            except OSError:
                continue
            p = LinkParser()
            try:
                p.feed(src)
            except Exception:
                continue
            if not p.match and not p.mismatch:
                continue
            rel = os.path.relpath(path, os.path.join(WPT_DIR, "css"))
            yield rel, p.match, p.mismatch, p.fuzzy


def resolve_ref(test_rel, href):
    """Resolve a ref href to a WPT-root-relative path (or None)."""
    href = href.split("#", 1)[0].split("?", 1)[0]
    if not href:
        return None
    if href.startswith("/"):
        return href[1:]
    return posixpath.normpath(posixpath.join("css", posixpath.dirname(test_rel), href))


def render(rel_url, out_ppm):
    """Render one WPT page (root-relative path) to a PPM; returns (ok, stderr_tail)."""
    env = dict(os.environ, WB_QUIET="1")
    url = f"http://127.0.0.1:{PORT}/{urllib.parse.quote(rel_url)}"
    for attempt in range(3):          # the local server drops a request now and then under load: render again
        try:
            r = subprocess.run([WBROWSE, url, str(VW), str(VH), out_ppm, RENDER_SECONDS],
                               env=env, capture_output=True, text=True, timeout=90)
        except subprocess.TimeoutExpired:
            return False, "render timeout"
        if r.returncode != 0 or not os.path.exists(out_ppm):
            if attempt < 2:
                continue
            return False, (r.stderr or r.stdout or "")[-200:]
        return True, ""
    return True, ""


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    os.remove(path)
    if not data.startswith(b"P6"):
        raise ValueError("not a PPM")
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    if len(px) < w * h * 3:
        raise ValueError("short PPM")
    return w, h, px


def diff_pixels(px_a, px_b, max_diff):
    """Count pixels where any channel differs by more than max_diff."""
    n = min(len(px_a), len(px_b))
    cnt = 0
    for i in range(0, n, 3):
        a, b = px_a[i:i + 3], px_b[i:i + 3]
        if (abs(a[0] - b[0]) > max_diff or abs(a[1] - b[1]) > max_diff or
                abs(a[2] - b[2]) > max_diff):
            cnt += 1
    return cnt


def run_one(job):
    """Render test + refs, compare. Returns a result dict."""
    test_rel, matches, mismatches, fuzzy = job
    tmp = os.path.join(REPO, "build-host", "wpt_tmp")
    os.makedirs(tmp, exist_ok=True)
    tag = re.sub(r"[^A-Za-z0-9_.-]", "_", test_rel)[-140:]

    def parse_fuzzy(*pages):
        md, tot = 0, 0
        for rel in pages:
            if not rel:
                continue
            p = LinkParser()
            try:
                path = os.path.join(WPT_DIR, "css", rel)
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    p.feed(f.read(262144))
            except Exception:
                continue
            if p.fuzzy:
                m = FUZZ_RE.search(p.fuzzy)
                if m:
                    md = max(md, int(m.group(2)))
                    tot = max(tot, int(m.group(4)))
        return md, tot

    fuzzy_page = [test_rel] + [resolve_ref(test_rel, h) for h in matches + mismatches]
    max_diff, total_allow = parse_fuzzy(*fuzzy_page)

    ok, err = render("css/" + test_rel, os.path.join(tmp, f"t_{tag}.ppm"))
    if not ok:
        return {"test": test_rel, "verdict": "error", "why": "test render: " + err}
    try:
        wa, ha, pxa = read_ppm(os.path.join(tmp, f"t_{tag}.ppm"))
    except Exception as e:
        return {"test": test_rel, "verdict": "error", "why": f"test ppm: {e}"}

    def try_ref(href, want_match):
        ref = resolve_ref(test_rel, href)
        if not ref:
            return {"test": test_rel, "verdict": "error", "why": f"empty ref href {href}"}
        if not (ref.endswith(".html") or (XHT and ref.endswith((".xht", ".xhtml", ".htm")))):
            return {"test": test_rel, "verdict": "skip", "why": f"non-html ref {ref}"}
        rp = os.path.join(tmp, f"r_{tag}_{re.sub(r'[^A-Za-z0-9_.-]', '_', ref)[-100:]}.ppm")
        if not os.path.exists(os.path.join(WPT_DIR, ref)):
            return {"test": test_rel, "verdict": "error", "why": f"missing ref file {ref}"}
        ok, err = render(ref, rp)
        if not ok:
            return {"test": test_rel, "verdict": "error", "why": f"ref render {ref}: {err}"}
        try:
            wb, hb, pxb = read_ppm(rp)
        except Exception as e:
            return {"test": test_rel, "verdict": "error", "why": f"ref ppm {ref}: {e}"}
        if (wa, ha) != (wb, hb):
            return {"test": test_rel, "verdict": "error", "why": f"ref size {ref}"}
        ndiff = diff_pixels(pxa, pxb, max_diff)
        same = ndiff <= (total_allow if want_match else total_allow)
        # match: pass when identical; mismatch: pass when different
        passed = same if want_match else not same
        return {"test": test_rel, "verdict": "pass" if passed else "fail",
                "why": f"{ref}: {ndiff} differing px (max_diff={max_diff}, allowed={total_allow})"}

    # match refs: ANY matching ref passes the test; ALL mismatch refs must differ
    results = []
    for href in matches:
        r = try_ref(href, True)
        if r["verdict"] == "pass":
            return r
        results.append(r)
    for href in mismatches:
        r = try_ref(href, False)
        if r["verdict"] != "pass":
            return r
        results.append(r)
    if results:
        return {"test": test_rel,
                "verdict": results[0]["verdict"],
                "why": "; ".join(x["why"] for x in results)[:400]}
    return {"test": test_rel, "verdict": "skip", "why": "no refs"}


_srv = None


def start_server():
    global _srv
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=WPT_DIR)
    http.server.ThreadingHTTPServer.allow_reuse_address = True
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    _srv = srv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("areas", nargs="+", help="css/ subdirectories to run")
    ap.add_argument("-j", type=int, default=4)
    ap.add_argument("-o", "--out", default=os.path.join(REPO, "build-host", "wpt_out"))
    ap.add_argument("--limit", type=int, default=0, help="cap per area (debug)")
    ap.add_argument("--list", action="store_true", help="only count discovered reftests")
    args = ap.parse_args()

    jobs = []
    for area in args.areas:
        got = list(discover(area))
        if args.limit:
            got = got[:args.limit]
        jobs.extend(got)
    print(f"discovered {len(jobs)} reftests across {len(args.areas)} areas")
    if args.list:
        for area in args.areas:
            n = len(list(discover(area)))
            print(f"  {area}: {n}")
        return 0

    start_server()
    os.makedirs(args.out, exist_ok=True)

    results = []
    with Pool(args.j) as pool:
        for i, res in enumerate(pool.imap_unordered(_run_job_global, jobs, chunksize=4)):
            results.append(res)
            if (i + 1) % 100 == 0:
                p = sum(1 for r in results if r["verdict"] == "pass")
                print(f"  {i+1}/{len(jobs)}  pass={p}")

    by_area = {}
    for r in results:
        area = r["test"].split("/")[0]
        st = by_area.setdefault(area, {"pass": 0, "fail": 0, "error": 0, "skip": 0})
        st[r["verdict"]] += 1

    lines = []
    tot = {"pass": 0, "fail": 0, "error": 0, "skip": 0}
    for area in sorted(by_area):
        st = by_area[area]
        for k in tot:
            tot[k] += st[k]
        run = st["pass"] / max(1, st["pass"] + st["fail"] + st["error"])
        lines.append(f"{area:28s} pass {st['pass']:4d}  fail {st['fail']:4d}  error {st['error']:3d}  skip {st['skip']:3d}  ({run*100:.1f}%)")
    lines.append(f"{'TOTAL':28s} pass {tot['pass']}  fail {tot['fail']}  error {tot['error']}  skip {tot['skip']}  ({tot['pass']/max(1,tot['pass']+tot['fail']+tot['error'])*100:.1f}%)")
    report = "\n".join(lines)
    print(report)
    with open(os.path.join(args.out, "summary.txt"), "w") as f:
        f.write(report + "\n")
    fails = [r for r in results if r["verdict"] in ("fail", "error")]
    with open(os.path.join(args.out, "results.json"), "w") as f:
        json.dump(results, f, indent=1)
    with open(os.path.join(args.out, "fails.txt"), "w") as f:
        for r in fails:
            f.write(f"{r['verdict'][0].upper()} {r['test']}  {r['why'][:200]}\n")
    return 0


def _run_job_global(job):
    return run_one(job)


if __name__ == "__main__":
    sys.exit(main())
