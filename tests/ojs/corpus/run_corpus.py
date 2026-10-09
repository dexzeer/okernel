#!/usr/bin/env python3
"""Differential corpus: real libraries exercised in node and in ojs, outputs diffed.

    python3 tests/ojs/corpus/run_corpus.py [-v] [x_name ...]

Each x_*.js names the libraries it needs (`// libs: a.js b.js`, optional
`// before: file.js` from this directory). The files run in one realm, in order:
common.js, before, libs, the exercise. ojs runs them with build-host/ojs_run;
node evaluates them with indirect eval (global scope, no CommonJS wrappers, so
UMD bundles take the same branch). Libraries are downloaded once into
build-host/corpus-lib (libs.txt) — test input only, never part of the kernel.
Run from WSL; node is node.exe through interop unless NODE is set.
"""
import difflib, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
LIB = os.path.join(ROOT, 'build-host', 'corpus-lib')
OJS = os.path.join(ROOT, 'build-host', 'ojs_run')
NODE = os.environ.get('NODE') or ('/mnt/c/Program Files/nodejs/node.exe' if os.path.exists('/mnt/c/Program Files/nodejs/node.exe') else 'node')
DRIVER = os.path.join(HERE, 'node_driver.js')

def winpath(p):
    if not NODE.endswith('.exe'):
        return p
    return subprocess.check_output(['wslpath', '-w', p], text=True).strip()

def fetch_libs():
    os.makedirs(LIB, exist_ok=True)
    for line in open(os.path.join(HERE, 'libs.txt')):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        name, url = line.split()
        dst = os.path.join(LIB, name)
        if not os.path.exists(dst) or os.path.getsize(dst) == 0:
            print('fetching', name)
            subprocess.check_call(['curl', '-sSL', '-o', dst, url])

def header(path, key):
    out = []
    for line in open(path, encoding='utf-8'):
        if not line.startswith('//'):
            break
        m = re.match(r'//\s*' + key + r':\s*(.*)', line)
        if m:
            out += m.group(1).split()
    return out

def files_for(x):
    path = os.path.join(HERE, x)
    return ([os.path.join(HERE, 'common.js')] + [os.path.join(HERE, b) for b in header(path, 'before')] +
            [os.path.join(LIB, l) for l in header(path, 'libs')] + [path])

def run(cmd, timeout=600):
    t = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        out = p.stdout.decode('utf-8', 'replace') + p.stderr.decode('utf-8', 'replace')
        return out, p.returncode, time.time() - t
    except subprocess.TimeoutExpired:
        return 'TIMEOUT', -1, time.time() - t

def normalize(text):
    lines = text.replace('\r\n', '\n').rstrip('\n').split('\n')
    return [l for l in lines if l.strip()]

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    verbose = '-v' in sys.argv
    fetch_libs()
    tests = sorted(f for f in os.listdir(HERE) if f.startswith('x_') and f.endswith('.js'))
    if args:
        tests = [t for t in tests if any(a in t for a in args)]
    bad = 0
    for x in tests:
        fs = files_for(x)
        o_out, o_rc, o_t = run([OJS] + fs)
        n_out, n_rc, n_t = run([NODE, winpath(DRIVER)] + [winpath(f) for f in fs])
        a, b = normalize(n_out), normalize(o_out)
        ok = a == b and o_rc == 0
        print('%-20s %s  node %.1fs  ojs %.1fs (rc %d)' % (x, 'OK  ' if ok else 'DIFF', n_t, o_t, o_rc))
        if not ok:
            bad += 1
            diff = list(difflib.unified_diff(a, b, 'node', 'ojs', lineterm='', n=0))
            for l in diff[:80 if verbose else 24]:
                print('    ' + l[:400])
    print('%d/%d identical' % (len(tests) - bad, len(tests)))
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
