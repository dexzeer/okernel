#!/usr/bin/env python3
"""okai: <img loading=lazy> is fetched only when its box comes within reach of the viewport (offline, local server on :8004).

The page has an eager image at the top, a lazy image right below it and a lazy image 4000 px further down. After the load only
the first two may have been requested; scrolling with the mouse wheel brings the far one in reach and it must be requested then.
"""
import sys, os, time, re, threading, zlib, struct
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

def png(r, g, b):
    raw = b''.join(b'\x00' + bytes([r, g, b]) * 8 for _ in range(8))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 8, 8, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')

PAGE = b"""<!doctype html><html><head><title>lazy</title></head><body style="margin:0">
<img src="/eager.png" width=100 height=100>
<img src="/near.png" loading=lazy width=100 height=100>
<div style="height:4000px;background:#eee">spacer</div>
<img src="/far.png" loading=lazy width=100 height=100>
<div style="height:600px"></div>
</body></html>"""

seen = []
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path == '/':
            body, ct = PAGE, 'text/html'
        elif self.path.endswith('.png'):
            seen.append(self.path); body, ct = png(200, 40, 40), 'image/png'
        else:
            self.send_response(404); self.send_header('Content-Length', '0'); self.end_headers(); return
        self.send_response(200); self.send_header('Content-Type', ct); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)

srv = ThreadingHTTPServer(('0.0.0.0', 8004), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
results = []
def step(name, ok):
    results.append((name, bool(ok))); print(('PASS ' if ok else 'FAIL ') + name, flush=True)

tag = sys.argv[1] if len(sys.argv) > 1 else 'okailazy'
vm = OkVM(tag)
try:
    vm.wait_for('[mem] heap', timeout=60)
    time.sleep(10)
    want = 'okai http://10.0.2.2:8004/'
    for attempt in range(4):
        vm.type_string(want + '\n', delay=0.05)
        time.sleep(4)
        if '[okai] fetch http://10.0.2.2:8004/' in vm.serial(): break
        print('  (command garbled, retrying)'); vm.type_string('\n', delay=0.05); time.sleep(2)
    vm.wait_for('sub-res done', timeout=90)
    time.sleep(5)
    step('the eager image was fetched', '/eager.png' in seen)
    step('the lazy image near the top was fetched', '/near.png' in seen)
    step('the lazy image far below was NOT fetched yet', '/far.png' not in seen)
    for _ in range(60):
        vm.mon('mouse_move 0 0 -1', 0.25)           # wheel down
        if '/far.png' in seen: break
    time.sleep(6)
    step('scrolling brings the far image in reach: it is fetched', '/far.png' in seen)
    log = vm.serial()
    step('no faults', not re.search(r'PAGE FAULT|\[ISR\] Exception|kmalloc FAIL', log))
finally:
    vm.kill()
    srv.shutdown()
print('RESULT:', 'PASS' if results and all(r for _, r in results) else 'FAIL')
sys.exit(0 if results and all(r for _, r in results) else 1)
