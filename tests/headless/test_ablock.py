#!/usr/bin/env python3
"""The built-in adblocker (ablock.c) end to end.

Run with the lists built with the test entries appended:
    make desktop ABLOCK_EXTRA=tests/headless/ablock_test.txt
    OKVM_ISO=kanarchy-desktop.iso python3 tests/headless/test_ablock.py

A local server (:8031) serves a page that pulls /ok.js (must load),
/ad.js + /banner.png + /track.gif (XHR) + an iframe /badframe/ (must never
reach the server), and paints a blue div behind two ad divs that the
cosmetic rules must hide. The screenshot must be blue at the ad slots.
"""
import sys, os, time, re, threading, struct, zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

counts = {}
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        counts[self.path] = counts.get(self.path, 0) + 1
        if self.path == '/': body, ct = PAGE, 'text/html'
        elif self.path == '/ok.js': body, ct = b"document.title='okloaded';", 'application/javascript'
        elif self.path == '/ok.css': body, ct = b'body{background:#fff}', 'text/css'
        elif self.path == '/ad.js': body, ct = b'window.ad=1;', 'application/javascript'
        elif self.path == '/banner.png': body, ct = png(255, 0, 255), 'image/png'
        elif self.path == '/track.gif': body, ct = b'GIF89a', 'image/gif'
        elif self.path == '/gtm.js': body, ct = b'window.__gtmShouldNotLoad=1;', 'application/javascript'
        elif self.path.startswith('/badframe/'): body, ct = b'<html><body>ad frame</body></html>', 'text/html'
        else:
            self.send_response(404); self.send_header('Content-Length', '0'); self.end_headers(); return
        self.send_response(200); self.send_header('Content-Type', ct); self.send_header('Content-Length', str(len(body)))
        self.end_headers(); self.wfile.write(body)

def png(r, g, b):
    raw = b''.join(b'\x00' + bytes([r, g, b]) * 8 for _ in range(8))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 8, 8, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')

PAGE = (b'<!doctype html><html><head><title>ablock</title>'
        b'<link rel=stylesheet href="/ok.css"><script src="/ok.js"></script>'
        b'<script src="/ad.js"></script></head>'
        b'<body style="margin:0">'
        b'<div class="ad-banner" style="width:200px;height:100px;background:#ff00ff">ad</div>'
        b'<div id="topad" style="width:200px;height:100px;background:#00ff00">tad</div>'
        b'<div style="width:200px;height:100px;background:#0000ff">ok</div>'
        b'<div class="ht-elem" style="width:200px;height:100px;background:#ff8000">win a FREE_IPHONE now</div>'
        b'<img src="/banner.png" width="120" height="40">'
        b'<iframe src="/badframe/x.html" width="120" height="40"></iframe>'
        b'<div id="s1" style="width:100px;height:50px;background:#111111"></div>'
        b'<div id="s2" style="width:100px;height:50px;background:#111111"></div>'
        b'<div id="s3" style="width:100px;height:50px;background:#111111"></div>'
        b'<script src="/gtm.js" onload="window.__gtmLoaded=1"></script>'
        b'<script>'
        b'var s1 = document.getElementById("s1"), s2 = document.getElementById("s2"),'
        b'    s3 = document.getElementById("s3");'
        b'if (window.__ablkOk === 1) s1.style.background = "#00ffff";'
        b'try { if (document.__ablkTrap) {} } catch (e) { s2.style.background = "#ffff00"; }'
        b'if (window.__gtmLoaded) s3.style.background = "#00ffff";'
        b'var x = new XMLHttpRequest(); x.open("GET", "/track.gif"); x.send();'
        b'</script>'
        b'</body></html>')

srv = ThreadingHTTPServer(('0.0.0.0', 8031), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
results = []
def step(name, ok):
    results.append((name, bool(ok))); print(('PASS ' if ok else 'FAIL ') + name, flush=True)

def pixel(w, h, data, x, y):
    o = (y * w + x) * 3
    return (data[o], data[o + 1], data[o + 2])

def count_color(w, h, data, rgb):
    n = 0
    for o in range(0, len(data) - 3, 3):
        if data[o] == rgb[0] and data[o + 1] == rgb[1] and data[o + 2] == rgb[2]: n += 1
    return n

tag = sys.argv[1] if len(sys.argv) > 1 else 'ablock'
vm = OkVM(tag)
try:
    log0 = vm.wait_for('[mem] heap', timeout=60)
    vm.wait_for('[ablk] filter module', timeout=30)
    vm.wait_for('[ablk] ', timeout=60)          # the load summary (or the finishing line)
    time.sleep(5)
    log = vm.serial()
    step('filter module loaded', '[ablk] filter module' in log)
    step('lists parsed', re.search(r'\[ablk\] \d+ network filters', log) is not None)
    vm.type_string('okai http://10.0.2.2:8031/\n', delay=0.05)
    vm.wait_for('parse: count=', timeout=90)
    time.sleep(8)
    log = vm.serial()
    t0 = time.time()
    while '[ablk] block JS' not in vm.serial() and time.time() - t0 < 30: time.sleep(0.5)
    log = vm.serial()
    step('/ad.js blocked', '[ablk] block JS' in log and '8031/ad.js' in log.split('[ablk] block JS', 1)[1][:400])
    step('/banner.png blocked', re.search(r'\[ablk\] block IMG \S*banner\.png', log) is not None)
    step('/track.gif (XHR) blocked', re.search(r'\[ablk\] block XHR \S*track\.gif', log) is not None)
    step('/badframe iframe blocked', re.search(r'\[ablk\] block FRAME \S*badframe', log) is not None)
    step('the page itself was fetched once', counts.get('/', 0) == 1)
    step('/ok.js loaded (control)', counts.get('/ok.js', 0) >= 1)
    step('/ok.css loaded (control)', counts.get('/ok.css', 0) >= 1)
    step('no ad request reached the server',
         counts.get('/ad.js', 0) == 0 and counts.get('/banner.png', 0) == 0 and
         counts.get('/track.gif', 0) == 0 and counts.get('/badframe/x.html', 0) == 0 and
         counts.get('/gtm.js', 0) == 0)
    step('gtm.js served from the built-in noop resource',
         re.search(r'\[ablk\] redirect \S*gtm\.js', log) is not None)
    # cosmetic: the two ad divs are display:none, so the blue div sits at the
    # top of the page area. Whole-screen color scans (no chrome offsets).
    w, h, data = vm.dump()
    def cnt(rgb): return count_color(w, h, data, rgb)
    step('ad divs hidden (blue div at the top)',
         cnt((255, 0, 255)) == 0 and cnt((0, 255, 0)) == 0 and cnt((0, 0, 255)) > 5000)
    step('blocked image never painted', cnt((255, 0, 255)) == 0)
    step('has-text hid only the matching div',
         cnt((255, 128, 0)) == 0 and cnt((0, 0, 255)) > 5000)     # FREE_IPHONE div gone, blue still there
    step('scriptlet set-constant ran (s1 cyan)', cnt((0, 255, 255)) >= 5000)
    step('scriptlet acs trap threw (s2 yellow)', cnt((255, 255, 0)) >= 2500)
    step('redirect script loaded (s3 cyan)', cnt((0, 255, 255)) >= 10000)  # s1 + s3
    step('no faults', not re.search(r'PAGE FAULT|\[ISR\] Exception|kmalloc FAIL', log))
    print('  server counts:', counts)
finally:
    vm.kill()
    srv.shutdown()
print('RESULT:', 'PASS' if results and all(r for _, r in results) else 'FAIL')
sys.exit(0 if results and all(r for _, r in results) else 1)
