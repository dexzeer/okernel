#!/usr/bin/env python3
"""Run test262 against the ojs engine (build-host/ojs_run).

    python3 tests/ojs/run262.py [-j N] [--t262 DIR] [--list-fails FILE] [subdir ...]

Each test runs in a fresh runtime (sloppy and strict unless flagged),
with the standard harness and its includes. Prints a summary per
directory; --list-fails writes every failing test with its reason.
"""
import os, re, sys, select, subprocess, threading, queue, collections, argparse, time

ap = argparse.ArgumentParser()
ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
ap.add_argument("--t262", default=os.path.expanduser("~/test262"))
ap.add_argument("--list-fails", default=None)
ap.add_argument("--timeout", type=float, default=10.0)
ap.add_argument("--bin", default="build-host/ojs_run")
ap.add_argument("--depth", type=int, default=3, help="directory depth of the summary")
ap.add_argument("--from-fails", default=None, help="rerun the tests listed in a --list-fails file")
ap.add_argument("--match", default=None, help="only reasons matching this regex (with --from-fails)")
ap.add_argument("dirs", nargs="*")
args = ap.parse_args()

T262 = args.t262
HARNESS = os.path.join(T262, "harness")
DIRS = args.dirs or ["test/language", "test/built-ins", "test/annexB"]
if args.from_fails:
    sel = []
    for l in open(args.from_fails, encoding="utf-8", errors="replace"):
        rel = l.split(" [", 1)[0]
        if args.match and not re.search(args.match, l):
            continue
        if rel not in sel:
            sel.append(rel)
    DIRS = sel

# proposals / host features outside the engine's scope
SKIP_FEATURES = {
    "Temporal", "ShadowRealm", "decorators", "source-phase-imports", "source-phase-imports-module-source",
    "import-defer", "explicit-resource-management", "json-modules", "import-text", "import-bytes",
    "IsHTMLDDA", "cross-realm", "Atomics.waitAsync", "canonical-tz", "Intl", "intl-normative-optional",
    "regexp-v-flag-properties-of-strings", "tail-call-optimization", "host-gc-required",
    "legacy-regexp", "Math.sumPrecise", "upsert", "iterator-sequencing", "joint-iteration", "await-dictionary",
    "Array.fromAsync", "uint8array-base64", "immutable-arraybuffer", "nonextensible-applies-to-private",
    "iterator-includes", "iterator-chunking", "Iterator.prototype.join", "caller",
}

def meta(src):
    m = re.search(r"/\*---(.*?)---\*/", src, re.S)
    y = m.group(1) if m else ""
    def listfield(name):
        f = re.search(name + r":\s*\[(.*?)\]", y, re.S)
        if f:
            return [x.strip() for x in f.group(1).split(",") if x.strip()]
        f = re.search(name + r":\s*\n((?:\s+-.*\n)+)", y)
        if f:
            return [l.strip()[1:].strip() for l in f.group(1).splitlines() if l.strip()]
        return []
    neg = re.search(r"negative:\s*\n\s*phase:\s*(\w+)\s*\n\s*type:\s*(\w+)", y)
    return listfield("flags"), listfield("features"), listfield("includes"), (neg.group(1), neg.group(2)) if neg else None

jobs = []
skipped = 0
for sub in DIRS:
    base = os.path.join(T262, sub)
    paths = [base] if base.endswith(".js") else []
    if not paths:
        for root, _, files in os.walk(base):
            for fn in sorted(files):
                if fn.endswith(".js") and "_FIXTURE" not in fn:
                    paths.append(os.path.join(root, fn))
    for p in sorted(paths):
        src = open(p, encoding="utf-8", errors="replace").read()
        flags, feats, incs, neg = meta(src)
        if SKIP_FEATURES & set(feats) or "atomicsHelper.js" in incs:   # multi-agent tests need threads
            skipped += 1
            continue
        fl = ""
        if "raw" in flags: fl += "r"
        if "async" in flags: fl += "a"
        if "module" in flags: modes = ["m"]
        elif "onlyStrict" in flags: modes = ["s"]
        elif "noStrict" in flags or "raw" in flags: modes = [""]
        else: modes = ["", "s"]
        negs = ("n" + neg[1]) if neg else ""
        for md in modes:
            jobs.append((md + fl + negs, p, ",".join(incs), "async" in flags))

print(f"{len(jobs)} runs ({skipped} tests skipped by feature)", flush=True)

results = [None] * len(jobs)
q = queue.Queue()
for i in range(len(jobs)):
    q.put(i)

def start():
    return subprocess.Popen([args.bin, "--test262", HARNESS], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, bufsize=0)

def worker():
    proc = start()
    buf = b""
    while True:
        try:
            i = q.get_nowait()
        except queue.Empty:
            break
        flags, path, incs, is_async = jobs[i]
        line = f"{flags or '-'} {path}" + (f" {incs}" if incs else "") + "\n"
        try:
            proc.stdin.write(line.encode())
        except BrokenPipeError:
            proc = start()
            buf = b""
            proc.stdin.write(line.encode())
        out_lines = []
        verdict = None
        deadline = time.time() + args.timeout
        while verdict is None:
            while b"\n" in buf:
                l, buf = buf.split(b"\n", 1)
                s = l.decode("utf-8", "replace")
                if s == "PASS" or s.startswith("FAIL"):
                    verdict = s
                    break
                out_lines.append(s)
            if verdict:
                break
            left = deadline - time.time()
            if left <= 0:
                verdict = "FAIL timeout"
                proc.kill()
                proc = start()
                buf = b""
                break
            r, _, _ = select.select([proc.stdout], [], [], left)
            if r:
                chunk = os.read(proc.stdout.fileno(), 65536)
                if not chunk:
                    verdict = "FAIL crash"
                    proc = start()
                    buf = b""
                    break
                buf += chunk
        if verdict == "PASS" and is_async:
            text = "\n".join(out_lines)
            if "Test262:AsyncTestComplete" not in text:
                fail = [l for l in out_lines if "AsyncTestFailure" in l]
                verdict = "FAIL async: " + (fail[0] if fail else "no completion")
        results[i] = verdict
    try:
        proc.stdin.close()
        proc.kill()
    except Exception:
        pass

threads = [threading.Thread(target=worker) for _ in range(args.j)]
t0 = time.time()
for t in threads: t.start()
for t in threads: t.join()

per_dir = collections.defaultdict(lambda: [0, 0])
fails = []
for (flags, path, incs, _), r in zip(jobs, results):
    rel = os.path.relpath(path, T262)
    key = "/".join(rel.split("/")[:args.depth])
    per_dir[key][1] += 1
    if r == "PASS":
        per_dir[key][0] += 1
    else:
        fails.append((rel, flags, r))
total_pass = sum(v[0] for v in per_dir.values())
for k in sorted(per_dir):
    p, n = per_dir[k]
    if p != n:
        print(f"{k:60s} {p:6d}/{n:<6d} {100.0 * p / n:6.1f}%")
print(f"TOTAL {total_pass}/{len(jobs)} = {100.0 * total_pass / max(1, len(jobs)):.2f}%  ({time.time() - t0:.0f}s)")
if args.list_fails:
    with open(args.list_fails, "w") as f:
        for rel, flags, r in fails:
            f.write(f"{rel} [{flags}] {r}\n")
