#!/usr/bin/env python3
"""Side-by-side comparison: okernel web engine vs Microsoft Edge (headless).

    python3 tests/web/compare.py <corpus-name> [width] [height]

Renders the corpus page with build-host/wrender (offline, from the corpus
manifest) and screenshots the page's live URL with Edge (light color scheme),
then writes tests/web/out/cmp_<name>.png (left: okernel, right: Edge).
Must run under WSL with access to the Windows Edge binary.
"""
import os, subprocess, sys
from PIL import Image

EDGE = '/mnt/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'

def offline_copy(name, page_url):
    """Corpus page with resource URLs rewritten to the local corpus files."""
    import re, html, urllib.parse
    d = 'tests/web/corpus/%s' % name
    files = {}
    for line in open(d + '/manifest.tsv'):
        p = line.rstrip('\n').split('\t')
        if p[0] != 'PAGE' and len(p) >= 2:
            files[p[0]] = p[1]
    src = open(d + '/index.html', 'rb').read().decode('utf-8', 'replace')
    base = page_url
    m = re.search(r'<base\s[^>]*href=["\']([^"\']+)', src, re.I)
    if m:
        base = urllib.parse.urljoin(page_url, html.unescape(m.group(1)))
    windir = subprocess.run(['wslpath', '-w', os.path.abspath(d)], capture_output=True, text=True).stdout.strip()
    def sub(mo):
        attr, q, val = mo.group(1), mo.group(2), mo.group(3)
        absu = urllib.parse.urljoin(base, html.unescape(val))
        if absu in files:
            return '%s=%s%s%s' % (attr, q, 'file:///' + (windir + '\\' + files[absu]).replace('\\', '/'), q)
        if val.startswith(('http:', 'https:', '//', 'data:', '#', 'javascript:', 'mailto:')):
            return mo.group(0)
        return '%s=%s%s%s' % (attr, q, absu, q)
    out = re.sub(r'\b(href|src)=(["\'])([^"\']*)\2', sub, src)
    out = re.sub(r'<base\s[^>]*>', '', out, flags=re.I)
    path = 'tests/web/out/ref/%s.html' % name
    open(path, 'w', encoding='utf-8').write(out)
    return path

def main():
    name = sys.argv[1]
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 1280
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 800
    out = 'tests/web/out'
    os.makedirs(out + '/ref', exist_ok=True)
    ours = '%s/%s.ppm' % (out, name)
    r = subprocess.run(['./build-host/wrender', name, str(w), str(h), ours, str(h)], capture_output=True, text=True)
    print(r.stdout.strip(), r.stderr.strip()[-500:])
    url = None
    for line in open('tests/web/corpus/%s/manifest.tsv' % name):
        p = line.rstrip('\n').split('\t')
        if p[0] == 'PAGE':
            url = p[1]
    ref = '%s/ref/%s.png' % (out, name)
    winref = subprocess.run(['wslpath', '-w', os.path.abspath(ref)], capture_output=True, text=True).stdout.strip()
    if url and (not os.path.exists(ref) or '--fresh' in sys.argv):
        local = offline_copy(name, url)
        winlocal = subprocess.run(['wslpath', '-w', os.path.abspath(local)], capture_output=True, text=True).stdout.strip()
        prof = subprocess.run(['wslpath', '-w', os.path.abspath(out + '/edgeprof')], capture_output=True, text=True).stdout.strip()
        subprocess.run([EDGE, '--headless=new', '--disable-gpu', '--hide-scrollbars', '--user-data-dir=' + prof,
                        '--allow-file-access-from-files', '--blink-settings=scriptEnabled=false,preferredColorScheme=1',
                        '--window-size=%d,%d' % (w, h), '--screenshot=' + winref,
                        'file:///' + winlocal.replace('\\', '/')],
                       capture_output=True, timeout=100)
    a = Image.open(ours).convert('RGB')
    try:
        b = Image.open(ref).convert('RGB')
    except Exception:
        b = Image.new('RGB', a.size, (128, 128, 128))
    W = a.width + b.width + 8
    H = max(a.height, b.height)
    c = Image.new('RGB', (W, H), (255, 0, 255))
    c.paste(a, (0, 0))
    c.paste(b, (a.width + 8, 0))
    dst = '%s/cmp_%s.png' % (out, name)
    c.save(dst)
    print('wrote', dst)

if __name__ == '__main__':
    main()
