#!/usr/bin/env python3
"""Run the ojs parser over test262: parse-phase negatives must fail, every
other test must parse. Usage:
    python3 tests/ojs/run_parse262.py [test262-dir] [subdir ...]
"""
import os, re, subprocess, sys, collections

T262 = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/test262")
SUBDIRS = sys.argv[2:] or ["test/language", "test/annexB/language", "test/built-ins"]
BIN = "build-host/t_parse"
# syntax we do not implement (proposals / later editions not targeted)
SKIP_FEATURES = {"decorators", "source-phase-imports", "import-defer", "explicit-resource-management",
                 "regexp-modifiers", "import-text", "import-bytes", "json-modules"}

def meta(src):
    m = re.search(r"/\*---(.*?)---\*/", src, re.S)
    y = m.group(1) if m else ""
    flags = re.search(r"flags:\s*\[(.*?)\]", y)
    flags = [f.strip() for f in flags.group(1).split(",")] if flags else []
    feats = re.search(r"features:\s*\[(.*?)\]", y, re.S)
    feats = [f.strip() for f in feats.group(1).split(",")] if feats else []
    if not feats:
        fm = re.search(r"features:\s*\n((?:\s+-.*\n)+)", y)
        if fm:
            feats = [l.strip()[1:].strip() for l in fm.group(1).splitlines()]
    neg = re.search(r"negative:\s*\n\s*phase:\s*(\w+)\s*\n\s*type:\s*(\w+)", y)
    return flags, feats, (neg.group(1), neg.group(2)) if neg else None

jobs = []
for sub in SUBDIRS:
    for root, _, files in os.walk(os.path.join(T262, sub)):
        for fn in files:
            if not fn.endswith(".js") or "_FIXTURE" in fn:
                continue
            p = os.path.join(root, fn)
            src = open(p, encoding="utf-8", errors="replace").read()
            flags, feats, neg = meta(src)
            if SKIP_FEATURES & set(feats):
                continue
            expect_err = neg is not None and neg[0] == "parse"
            if "module" in flags:
                modes = ["m"]
            elif "onlyStrict" in flags:
                modes = ["S"]
            elif "noStrict" in flags or "raw" in flags:
                modes = ["s"]
            else:
                modes = ["s", "S"]
            for md in modes:
                jobs.append((md, p, expect_err, feats))

proc = subprocess.run([BIN], input="\n".join(f"{m} {p}" for m, p, _, _ in jobs) + "\n",
                      capture_output=True, text=True)
out = proc.stdout.splitlines()
if len(out) != len(jobs):
    print(f"runner output mismatch: {len(out)} lines for {len(jobs)} jobs; stderr: {proc.stderr[-2000:]}")
    sys.exit(2)
fail_pos, fail_neg = [], []
by_dir = collections.Counter()
for (md, p, expect_err, feats), res in zip(jobs, out):
    got_err = res.startswith("ERR")
    if got_err != expect_err:
        rel = os.path.relpath(p, T262)
        (fail_neg if expect_err else fail_pos).append((md, rel, res, feats))
        by_dir[os.path.dirname(rel)] += 1
print(f"{len(jobs)} parses: {len(jobs) - len(fail_pos) - len(fail_neg)} ok, "
      f"{len(fail_pos)} valid code rejected, {len(fail_neg)} invalid code accepted")
print("worst directories:")
for d, n in by_dir.most_common(25):
    print(f"  {n:5d} {d}")
lim = int(os.environ.get("SHOW", "40"))
print("--- valid code rejected (sample):")
for md, rel, res, feats in fail_pos[:lim]:
    print(f"  [{md}] {rel}: {res[:160]}")
print("--- invalid code accepted (sample):")
for md, rel, res, feats in fail_neg[:lim]:
    print(f"  [{md}] {rel}")
with open("build-host/parse262_fail.txt", "w") as f:
    for md, rel, res, feats in fail_pos:
        f.write(f"POS [{md}] {rel}: {res}\n")
    for md, rel, res, feats in fail_neg:
        f.write(f"NEG [{md}] {rel} features={feats}\n")
