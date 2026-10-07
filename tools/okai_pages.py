#!/usr/bin/env python3
"""Export okai's built-in HTML pages (home page, error page template) from
src/okai.c into host-test corpus entries, so they can be rendered with
build-host/wrender and compared against Edge (tests/web/compare.py):

    python3 tools/okai_pages.py && python3 tests/web/compare.py okai_home 1916 936
"""
import ast, os, re

src = open('src/okai.c', encoding='utf-8').read()

def c_string(name):
    m = re.search(r'static const char %s\[\]\s*=\s*((?:\s*"(?:[^"\\]|\\.)*")+)\s*;' % name, src)
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    return ''.join(ast.literal_eval('"%s"' % p) for p in parts)

def write(name, html, url):
    d = 'tests/web/corpus/%s' % name
    os.makedirs(d, exist_ok=True)
    open(d + '/index.html', 'w', encoding='utf-8').write(html)
    open(d + '/manifest.tsv', 'w').write('PAGE\t%s\tindex.html\n' % url)
    print('wrote', d)

write('okai_home', c_string('OKAI_HOME_HTML'), 'http://okai.home/')
