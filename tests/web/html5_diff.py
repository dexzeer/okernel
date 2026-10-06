#!/usr/bin/env python3
"""Differential test: okernel HTML5 parser vs html5lib (scripting off).

Runs every case in tests/web/html5_cases.txt (cases separated by lines that
start with '#----') plus any extra files given on the command line through
build-host/html5_dump and html5lib, serializes both trees in the html5lib
test format, and reports mismatches. Exit status 1 on any mismatch.
"""
import subprocess, sys, os, tempfile, difflib
import html5lib

DUMP = os.path.join('build-host', 'html5_dump')
# html5lib 1.1 predates parts of the current WHATWG algorithm; where it and
# the living standard disagree we follow the standard (and Chrome).
KNOWN_SPEC_DIFFS = {
    'template': 'html5lib 1.1 puts a leading <template> in <body>; the spec and Chrome insert it in <head>',
}
NS = {'http://www.w3.org/1999/xhtml': '', 'http://www.w3.org/2000/svg': 'svg ',
      'http://www.w3.org/1998/Math/MathML': 'math '}

def ser(doc):
    out = []
    def text(t, depth):
        if t:
            out.append('| ' + '  ' * depth + '"%s"' % t)
    def rec(el, depth):
        tag = el.tag
        if tag is html5lib.treebuilders.etree.Comment if False else False:
            pass
        if callable(tag):  # comment
            out.append('| ' + '  ' * depth + '<!-- %s -->' % el.text)
            text(el.tail, depth)
            return
        if tag == '<!DOCTYPE>':
            text(el.tail, depth)
            return
        if tag.startswith('{'):
            ns, local = tag[1:].split('}', 1)
            prefix = NS.get(ns, '')
        else:
            prefix, local = '', tag
        out.append('| ' + '  ' * depth + '<%s%s>' % (prefix, local.lower()))
        attrs = []
        for k, v in el.attrib.items():
            if k.startswith('{'):
                k = k.split('}', 1)[1]
            attrs.append((k.lower(), v))
        for k, v in sorted(attrs):
            out.append('| ' + '  ' * (depth + 1) + '%s="%s"' % (k, v))
        text(el.text, depth + 1)
        for c in el:
            rec(c, depth + 1)
        if depth >= 0:
            pass
        text(el.tail, depth)
    root = doc
    # html5lib etree: document root is the <html> element; leading comments /
    # doctype are not reachable through it, so compare from <html> down and
    # strip document-level comments from our side too.
    rec(root, 0)
    # merge adjacent text lines produced by tail/text split
    return out

def ours(path):
    r = subprocess.run([DUMP, path], capture_output=True)
    lines = r.stdout.decode('utf-8', 'replace').split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    # drop document-level comments (html5lib's etree root hides them)
    lines = [l for l in lines if not (l.startswith('| <!--'))]
    return lines, r.stderr.decode('utf-8', 'replace').strip()

def theirs(data, enc=None):
    doc = html5lib.parse(data, treebuilder='etree', scripting=False, override_encoding=enc)
    lines = ser(doc)
    # html5lib may emit separate text lines for a node's text and tail that
    # belong to one DOM text node in our tree: merge consecutive text lines
    # at the same depth.
    merged = []
    for l in lines:
        if merged and l.lstrip('| ').startswith('"') and merged[-1].lstrip('| ').startswith('"'):
            a, b = merged[-1], l
            if len(a) - len(a.lstrip('| ')) == len(b) - len(b.lstrip('| ')):
                merged[-1] = a[:-1] + b.lstrip('| ')[1:]
                continue
        merged.append(l)
    return merged

def run_case(name, data):
    with tempfile.NamedTemporaryFile(suffix='.html', delete=False) as f:
        f.write(data)
        path = f.name
    try:
        mine, info = ours(path)
        enc = info.split('charset=')[1].split()[0] if 'charset=' in info else None
        ref = theirs(data, enc)
    finally:
        os.unlink(path)
    # compare as whole strings: text nodes may contain newlines
    a = '\n'.join(ref).split('\n')
    b = '\n'.join(mine).split('\n')
    if a == b:
        return True, info
    if name in KNOWN_SPEC_DIFFS:
        print('KNOWN: %s (%s)' % (name, KNOWN_SPEC_DIFFS[name]))
        return True, info
    diff = list(difflib.unified_diff(a, b, 'html5lib', 'okernel', lineterm='', n=2))
    print('MISMATCH: %s (%s)' % (name, info))
    print('\n'.join(diff[:60]))
    return False, info

def main():
    cases = []
    src = open('tests/web/html5_cases.txt', 'rb').read().split(b'\n#----')
    for i, c in enumerate(src):
        c = c.split(b'\n', 1)
        title = c[0].decode().strip(' -#') or 'case%d' % i
        body = c[1] if len(c) > 1 else b''
        cases.append((title, body))
    for p in sys.argv[1:]:
        cases.append((p, open(p, 'rb').read()))
    ok = 0
    for name, data in cases:
        good, _ = run_case(name, data)
        ok += good
    print('HTML5 DIFF: %d/%d match html5lib' % (ok, len(cases)))
    sys.exit(0 if ok == len(cases) else 1)

if __name__ == '__main__':
    main()
