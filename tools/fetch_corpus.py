#!/usr/bin/env python3
"""Fetch the real-world page corpus used by the web engine tests.

For each site: saves tests/web/corpus/<name>/index.html, every linked
stylesheet (<link rel=stylesheet>, recursive @import) and images referenced by
<img src> (first N), plus manifest.json mapping absolute URL -> local file.
The corpus is not committed (see .gitignore); rerun to refresh:
    python3 tools/fetch_corpus.py [name ...]
"""
import json, os, re, sys, urllib.request, urllib.parse, gzip, html

SITES = {
    'wikipedia_os': 'https://en.wikipedia.org/wiki/Operating_system',
    'wikipedia_portal': 'https://www.wikipedia.org/',
    'hn': 'https://news.ycombinator.com/',
    'example': 'https://example.com/',
    'mdn_display': 'https://developer.mozilla.org/en-US/docs/Web/CSS/display',
    'python_org': 'https://www.python.org/',
    'python_docs': 'https://docs.python.org/3/library/os.html',
    'danluu': 'https://danluu.com/',
    'cern': 'https://info.cern.ch/hypertext/WWW/TheProject.html',
    'lwn': 'https://lwn.net/',
    'archlinux': 'https://archlinux.org/',
    'gnu': 'https://www.gnu.org/',
    'npr_text': 'https://text.npr.org/',
    'github_linux': 'https://github.com/torvalds/linux',
    'bbc_news': 'https://www.bbc.com/news',
    'rust_lang': 'https://www.rust-lang.org/',
    'sqlite': 'https://sqlite.org/index.html',
    'kernel_org': 'https://www.kernel.org/',
}
UA = 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36'
MAX_IMG = 40

def get(url):
    req = urllib.request.Request(url, headers={'User-Agent': UA, 'Accept-Encoding': 'gzip',
                                               'Accept': '*/*', 'Accept-Language': 'en-US,en'})
    with urllib.request.urlopen(req, timeout=30) as r:
        data = r.read()
        if r.headers.get('Content-Encoding') == 'gzip':
            data = gzip.decompress(data)
        return data, r.headers.get('Content-Type', ''), r.geturl()

def safe_name(i, url, ext):
    base = re.sub(r'[^A-Za-z0-9._-]', '_', urllib.parse.urlparse(url).path.split('/')[-1])[:40]
    return '%03d_%s%s' % (i, base or 'res', ext if not base.endswith(ext) else '')

def fetch_site(name, url):
    out = os.path.join('tests', 'web', 'corpus', name)
    os.makedirs(out, exist_ok=True)
    manifest = {}
    try:
        page, ctype, final = get(url)
    except Exception as e:
        print('  FAIL', name, e)
        return
    open(os.path.join(out, 'index.html'), 'wb').write(page)
    manifest['__page__'] = {'url': final, 'file': 'index.html', 'type': ctype}
    text = page.decode('utf-8', 'replace')
    base = final
    m = re.search(r'<base\s[^>]*href=["\']([^"\']+)', text, re.I)
    if m:
        base = urllib.parse.urljoin(final, html.unescape(m.group(1)))
    css_urls = []
    for m in re.finditer(r'<link\b[^>]*>', text, re.I):
        tag = m.group(0)
        if not re.search(r'rel=["\']?[^"\'>]*stylesheet', tag, re.I):
            continue
        h = re.search(r'href=["\']?([^"\'\s>]+)', tag, re.I)
        if h:
            css_urls.append(urllib.parse.urljoin(base, html.unescape(h.group(1))))
    idx = 0
    seen = set()
    def fetch_css(u, depth=0):
        nonlocal idx
        if u in seen or depth > 3:
            return
        seen.add(u)
        try:
            data, ct, fu = get(u)
        except Exception as e:
            print('  css fail', u, e)
            return
        fn = safe_name(idx, u, '.css'); idx += 1
        open(os.path.join(out, fn), 'wb').write(data)
        manifest[u] = {'file': fn, 'type': ct}
        for im in re.finditer(rb'@import\s+(?:url\()?["\']?([^"\')\s;]+)', data):
            fetch_css(urllib.parse.urljoin(u, im.group(1).decode('utf-8', 'replace')), depth + 1)
    for u in css_urls:
        fetch_css(u)
    imgs = []
    for m in re.finditer(r'<img\b[^>]*\bsrc=["\']([^"\']+)', text, re.I):
        imgs.append(urllib.parse.urljoin(base, html.unescape(m.group(1))))
    for u in imgs[:MAX_IMG]:
        if u in manifest or u.startswith('data:'):
            continue
        try:
            data, ct, fu = get(u)
        except Exception as e:
            continue
        fn = safe_name(idx, u, ''); idx += 1
        open(os.path.join(out, fn), 'wb').write(data)
        manifest[u] = {'file': fn, 'type': ct}
    json.dump(manifest, open(os.path.join(out, 'manifest.json'), 'w'), indent=1)
    print('  %s: %d bytes html, %d resources' % (name, len(page), len(manifest) - 1))

def main():
    names = sys.argv[1:] or list(SITES)
    for n in names:
        print('fetch', n)
        fetch_site(n, SITES[n])

if __name__ == '__main__':
    main()
