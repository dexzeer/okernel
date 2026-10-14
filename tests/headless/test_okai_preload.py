#!/usr/bin/env python3
"""okai preload scanner (offline, local server on :8006).

The document is sent slowly: its first 5 KB (head with a stylesheet, scripts and an image) arrive at once, the rest 3 s later.
The sub-resources must be requested while the document is still downloading, each exactly once (the real request made after
parsing is answered from the cache or takes over the load in flight). Run for the three framings okai has to understand:
plain Content-Length, chunked, and gzip + chunked.
"""
import sys, os, time, re, threading, zlib, struct, gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

def png(r, g, b):
    raw = b''.join(b'\x00' + bytes([r, g, b]) * 8 for _ in range(8))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 8, 8, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')

# a head with real work to find, padded with comments (compressible, so the gzip variant still needs a few KB of stream)
import random
random.seed(5)
PAD = ''.join('<!-- %08x %08x %08x -->\n' % (random.getrandbits(32), random.getrandbits(32), random.getrandbits(32)) for _ in range(180))
HEAD = ('<!doctype html><html><head><title>preload</title>\n<link rel=stylesheet href="/p.css">\n<script src="/p1.js"></script>\n'
        '<link rel=preload as=script href="/p2.js">\n' + PAD + '</head><body><img src="/p.png" width=20 height=20>\n')
TAIL = '<p>the rest of the page</p>\n' + ''.join('<p>line %d</p>\n' % i for i in range(200)) + '</body></html>'
FULL = (HEAD + TAIL).encode()

times = {}      # path -> time of first request
counts = {}
doc_tail_at = [0.0]
mode = ['plain']

class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def do_GET(self):
        counts[self.path] = counts.get(self.path, 0) + 1
        times.setdefault(self.path, time.time())
        if self.path == '/':
            data = FULL
            gz = mode[0] == 'gzip'
            if gz:
                # one deflate stream cut in two flushes, so the first part is decodable on its own
                co = zlib.compressobj(6, zlib.DEFLATED, 31)
                head = HEAD.encode(); tail = TAIL.encode()
                p1 = co.compress(head) + co.flush(zlib.Z_SYNC_FLUSH)
                p2 = co.compress(tail) + co.flush()
                parts = [p1, p2]
            else:
                cut = len(HEAD.encode())
                parts = [FULL[:cut], FULL[cut:]]
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            if gz: self.send_header('Content-Encoding', 'gzip')
            chunked = mode[0] != 'plain'
            if chunked: self.send_header('Transfer-Encoding', 'chunked')
            else: self.send_header('Content-Length', str(len(FULL)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            def w(b):
                if chunked: self.wfile.write(b'%x\r\n' % len(b) + b + b'\r\n')
                else: self.wfile.write(b)
                self.wfile.flush()
            w(parts[0])
            time.sleep(3.0)
            doc_tail_at[0] = time.time()
            w(parts[1])
            if chunked: self.wfile.write(b'0\r\n\r\n'); self.wfile.flush()
            return
        body, ct = {'/p.css': (b'p { color: #333 }', 'text/css'), '/p1.js': (b'window.a = 1;', 'application/javascript'),
                    '/p2.js': (b'window.b = 2;', 'application/javascript'), '/p.png': (png(9, 99, 199), 'image/png')}.get(self.path, (None, None))
        if body is None:
            self.send_response(404); self.send_header('Content-Length', '0'); self.end_headers(); return
        self.send_response(200); self.send_header('Content-Type', ct); self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-cache'); self.end_headers(); self.wfile.write(body)

srv = ThreadingHTTPServer(('0.0.0.0', 8006), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
results = []
def step(name, ok):
    results.append((name, bool(ok))); print(('PASS ' if ok else 'FAIL ') + name, flush=True)

for m in (sys.argv[1:] or ['plain', 'chunked', 'gzip']):
    mode[0] = m
    times.clear(); counts.clear(); doc_tail_at[0] = 0
    vm = OkVM('okaipre_' + m)
    try:
        vm.wait_for('[mem] heap', timeout=60)
        time.sleep(10)
        want = 'okai http://10.0.2.2:8006/'
        for attempt in range(4):
            vm.type_string(want + '\n', delay=0.05)
            time.sleep(4)
            if '[okai] fetch http://10.0.2.2:8006/' in vm.serial(): break
            print('  (command garbled, retrying)'); vm.type_string('\n', delay=0.05); time.sleep(2)
        vm.wait_for('sub-res done', timeout=90)
        time.sleep(3)
        t_tail = doc_tail_at[0]
        for p in ('/p.css', '/p1.js', '/p2.js', '/p.png'):
            step('[%s] %s requested while the document was still downloading' % (m, p), p in times and t_tail and times[p] < t_tail)
            step('[%s] %s requested exactly once' % (m, p), counts.get(p) == 1)
        log = vm.serial()
        step('[%s] preload lines in the log' % m, len(re.findall(r'\[okai\] preload http://10\.0\.2\.2:8006/', log)) >= 4)
        step('[%s] the page still loaded its resources' % m, 'sub-res CSS: status=200' in log and 'sub-res IMG: status=200' in log)
        step('[%s] no faults' % m, not re.search(r'PAGE FAULT|\[ISR\] Exception|kmalloc FAIL', log))
        print('  counts', counts)
    finally:
        vm.kill()
print('RESULT:', 'PASS' if results and all(r for _, r in results) else 'FAIL')
srv.shutdown()
sys.exit(0 if results and all(r for _, r in results) else 1)
