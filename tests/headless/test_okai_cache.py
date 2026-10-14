#!/usr/bin/env python3
"""okai response cache (offline, local server on :8005).

Two pages share a stylesheet and an image served with max-age, a script served with no-store, and one image with no caching
headers at all. After visiting both, the cacheable assets must have been requested from the server once, the others twice;
the second page's log must show the cache hits.
"""
import sys, os, time, re, threading, zlib, struct
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

def png(r, g, b):
    raw = b''.join(b'\x00' + bytes([r, g, b]) * 8 for _ in range(8))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 8, 8, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')

def page(n):
    return ('<!doctype html><html><head><title>cache %d</title><link rel=stylesheet href="/a.css"><script src="/nostore.js"></script></head>'
            '<body><img src="/cached.png" width=40 height=40><img src="/plain.png" width=40 height=40>'
            '<p><a href="/page2">go to page 2</a> <a href="/">back</a></p></body></html>' % n).encode()

counts = {}
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        counts[self.path] = counts.get(self.path, 0) + 1
        hdr = {}
        if self.path == '/': body, ct = page(1), 'text/html'
        elif self.path == '/page2': body, ct = page(2), 'text/html'
        elif self.path == '/a.css': body, ct = b'p { color: #333 }', 'text/css'; hdr['Cache-Control'] = 'public, max-age=300'
        elif self.path == '/cached.png': body, ct = png(10, 120, 200), 'image/png'; hdr['Cache-Control'] = 'max-age=300, immutable'
        elif self.path == '/plain.png': body, ct = png(200, 120, 10), 'image/png'
        elif self.path == '/nostore.js': body, ct = b'window.x = 1;', 'application/javascript'; hdr['Cache-Control'] = 'no-store'
        else:
            self.send_response(404); self.send_header('Content-Length', '0'); self.end_headers(); return
        self.send_response(200); self.send_header('Content-Type', ct); self.send_header('Content-Length', str(len(body)))
        for k, v in hdr.items(): self.send_header(k, v)
        self.end_headers(); self.wfile.write(body)

srv = ThreadingHTTPServer(('0.0.0.0', 8005), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
results = []
def step(name, ok):
    results.append((name, bool(ok))); print(('PASS ' if ok else 'FAIL ') + name, flush=True)

tag = sys.argv[1] if len(sys.argv) > 1 else 'okaicache'
vm = OkVM(tag)
try:
    vm.wait_for('[mem] heap', timeout=60)
    time.sleep(10)
    want = 'okai http://10.0.2.2:8005/'
    for attempt in range(4):
        vm.type_string(want + '\n', delay=0.05)
        time.sleep(4)
        if '[okai] fetch http://10.0.2.2:8005/' in vm.serial(): break
        print('  (command garbled, retrying)'); vm.type_string('\n', delay=0.05); time.sleep(2)
    vm.wait_for('sub-res done', timeout=90)
    time.sleep(8)
    n = vm.serial().count('parse: count=')
    vm.type_string('g'); time.sleep(1)                 # focus the address bar
    vm.type_string('http://10.0.2.2:8005/page2\n', delay=0.05)
    step('navigated to page 2', vm.wait_for('[okai] navigate http://10.0.2.2:8005/page2', timeout=20))
    t0 = time.time()
    while vm.serial().count('parse: count=') <= n and time.time() - t0 < 60: time.sleep(0.5)
    time.sleep(8)
    log = vm.serial()
    step('/a.css fetched from the server once (max-age)', counts.get('/a.css') == 1)
    step('/cached.png fetched once (max-age, immutable)', counts.get('/cached.png') == 1)
    step('/plain.png fetched twice (no cache headers)', counts.get('/plain.png') == 2)
    step('/nostore.js fetched twice (no-store)', counts.get('/nostore.js') == 2)
    step('the log shows the hits', len(re.findall(r'\[cache\] hit \S*a\.css', log)) >= 1 and len(re.findall(r'\[cache\] hit \S*cached\.png', log)) >= 1)
    step('no faults', not re.search(r'PAGE FAULT|\[ISR\] Exception|kmalloc FAIL', log))
    print('  server counts:', counts)
finally:
    vm.kill()
    srv.shutdown()
print('RESULT:', 'PASS' if results and all(r for _, r in results) else 'FAIL')
sys.exit(0 if results and all(r for _, r in results) else 1)
