import re, html, os, json, sys
"""Usage: py tests/web/surface_make_cmp.py <edge dump-dom output of tests/web/js/surface.html>  ->  build-host/surface_cmp.js
Then (WSL): WB_EVAL=build-host/surface_cmp.js build-host/wbrowse tests/web/js/surface_inline.html 1000 700 /tmp/s.ppm 5
prints SUMMARY, MISSING|, EXTRA|, DIFF| (names Edge has and we lack, the reverse, and different kinds). Edge: msedge --headless=new --dump-dom file:///.../surface.html"""
t = open(sys.argv[1], encoding='utf-8', errors='replace').read()
m = re.search(r'<pre id="surface">(.*?)</pre>', t, re.S)
lines = [l for l in html.unescape(m.group(1)).split('\n') if l.strip()]
print(len(lines))
open(r'D:\projects\okernel\build-host\edge_surface.txt', 'w', encoding='utf-8').write('\n'.join(lines))
js = "var EDGE = " + json.dumps('\n'.join(lines)) + ";\n"
js += r"""
(function () {
    var ours = __surface.split('\n'), mine = {}, theirs = {};
    ours.forEach(function (l) { var i = l.lastIndexOf(' '); mine[l.slice(0, i)] = l.slice(i + 1); });
    EDGE.split('\n').forEach(function (l) { var i = l.lastIndexOf(' '); theirs[l.slice(0, i)] = l.slice(i + 1); });
    var missing = [], extra = [], diff = [];
    for (var k in theirs) { if (!(k in mine)) missing.push(k + ' ' + theirs[k]); else if (mine[k] !== theirs[k]) diff.push(k + ' edge=' + theirs[k] + ' ours=' + mine[k]); }
    for (var k in mine) if (!(k in theirs)) extra.push(k + ' ' + mine[k]);
    console.log('SUMMARY edge=' + Object.keys(theirs).length + ' ours=' + Object.keys(mine).length + ' missing=' + missing.length + ' extra=' + extra.length + ' kinddiff=' + diff.length);
    function out(tag, a) { for (var i = 0; i < a.length; i += 12) console.log(tag + '|' + a.slice(i, i + 12).join(' ;; ')); }
    out('MISSING', missing); out('EXTRA', extra); out('DIFF', diff);
})();
"""
open(r'D:\projects\okernel\build-host\surface_cmp.js', 'w', encoding='utf-8').write(js)
