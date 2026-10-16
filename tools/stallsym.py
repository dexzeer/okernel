#!/usr/bin/env python3
# Resolve the EIPs of `[stall]` lines (prof.c stall sampler) to kernel function names.
#   python3 tools/stallsym.py kanarchy-desktop.bin perf.log
import re, subprocess, sys, bisect
syms = []
for line in subprocess.check_output(['nm', '-n', sys.argv[1]], text=True).splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
def name(e):
    i = bisect.bisect_right(addrs, e) - 1
    return f"{syms[i][1]}+{e - syms[i][0]:#x}" if i >= 0 else hex(e)
tot = {}
for line in open(sys.argv[2], errors='replace'):
    if not line.startswith('[stall]'): continue
    m = re.match(r'\[stall\] (\d+) ms \((\d+) samples\)', line)
    print(line.strip()[:60])
    for e, w0, w1, n in re.findall(r'([0-9a-f]{6,8})<([0-9a-f]+),([0-9a-f]+)>\*(\d+)', line):
        f = name(int(e, 16)).split('+')[0]
        def sym(w):
            v = int(w, 16)
            return name(v) if 0xC0100000 <= v < addrs[-1] + 0x10000 else '-'
        key = f if sym(w0) == '-' else f + ' <- ' + sym(w0).split('+')[0]
        tot[key] = tot.get(key, 0) + int(n)
        print(f"    {int(n):4d}  {name(int(e, 16))}   [{sym(w0)}] [{sym(w1)}]")
print('--- total samples (10 ms each) per function')
for f, n in sorted(tot.items(), key=lambda x: -x[1])[:25]: print(f"{n:5d} {f}")
