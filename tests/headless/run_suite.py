#!/usr/bin/env python3
"""Run headless tests N at a time and summarize (each boots its own QEMU).

    python3 tests/headless/run_suite.py [-j3] test_a.py test_b.py ...

Output of each test goes to ~/okvm/<test>.out; the summary prints the tail
of each and the exit status.
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.expanduser("~/okvm")
os.makedirs(OUT, exist_ok=True)
jobs = 3
tests = []
for a in sys.argv[1:]:
    if a.startswith("-j"):
        jobs = int(a[2:])
    else:
        tests.append(a)
running, done = [], []
while tests or running:
    while tests and len(running) < jobs:
        t = tests.pop(0)
        f = open(os.path.join(OUT, os.path.basename(t) + ".out"), "w")
        p = subprocess.Popen([sys.executable, os.path.join(HERE, os.path.basename(t))],
                             stdout=f, stderr=subprocess.STDOUT, cwd=os.path.dirname(os.path.dirname(HERE)))
        running.append((t, p, f, time.time()))
        time.sleep(4)  # stagger boots
    for r in running[:]:
        t, p, f, t0 = r
        if p.poll() is not None or time.time() - t0 > 900:
            if p.poll() is None:
                p.kill()
            f.close()
            running.remove(r)
            done.append((t, p.returncode, time.time() - t0))
    time.sleep(2)
for t, rc, dt in done:
    tail = open(os.path.join(OUT, os.path.basename(t) + ".out"), errors="replace").read().strip().splitlines()[-4:]
    print(f"=== {t} rc={rc} ({dt:.0f}s)")
    for l in tail:
        print("   ", l[:160])
