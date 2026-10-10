#!/usr/bin/env python3
"""okai fetch()/XHR with methods, headers and bodies (offline, local server on :8003).

The page POSTs a JSON string, a binary Uint8Array (every byte value), a 40 KB body, sends custom
headers, uses PUT/DELETE, follows a 302 on a POST (becomes a GET) and a 307 (stays a POST); the
server echoes what it received and the page checks it.
"""
import sys, os, time, re, threading, json, hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

PAGE = b"""<!doctype html><html><head><title>post</title></head><body><div id=o>running</div><script>
const log = m => console.log('POSTTEST ' + m);
const bin = new Uint8Array(256 * 4); for (let i = 0; i < bin.length; i++) bin[i] = i & 255;
const big = 'x'.repeat(40000);
function sum(u8) { let s = 0; for (const b of u8) s = (s * 31 + b) >>> 0; return s; }
async function run() {
  let r = await fetch('/echo', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Test': 'one' }, body: JSON.stringify({ a: 1 }) });
  let j = await r.json(); log('json ' + j.method + ' ' + j.ctype + ' ' + j.xtest + ' ' + j.len + ' ' + j.body);
  r = await fetch('/echo', { method: 'POST', body: bin });
  const ab = await r.arrayBuffer(); const u = new Uint8Array(ab);
  log('binary len=' + u.length + ' sum=' + sum(u) + ' want=' + sum(bin));
  r = await fetch('/echo?mode=len', { method: 'POST', body: big });
  j = await r.json(); log('big len=' + j.len + ' ctype=' + j.ctype);
  r = await fetch('/echo', { method: 'PUT', body: 'hello' }); j = await r.json(); log('put ' + j.method + ' ' + j.len);
  r = await fetch('/echo', { method: 'DELETE' }); j = await r.json(); log('delete ' + j.method + ' ' + j.len);
  r = await fetch('/redir302', { method: 'POST', body: 'abc' }); j = await r.json(); log('302 ' + j.method + ' ' + j.len);
  r = await fetch('/redir307', { method: 'POST', body: 'abc' }); j = await r.json(); log('307 ' + j.method + ' ' + j.len);
  await new Promise(res => { const x = new XMLHttpRequest(); x.open('POST', '/echo'); x.setRequestHeader('Content-Type', 'text/x-test'); x.setRequestHeader('Host', 'evil'); x.onload = () => { const q = JSON.parse(x.responseText); log('xhr ' + q.method + ' ' + q.ctype + ' ' + q.len + ' host=' + q.host); res(); }; x.send('12345'); });
  log('DONE');
}
run().catch(e => log('FAILED ' + e));
</script></body></html>"""

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _do(self):
        n = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(n) if n else b''
        if self.path == '/':
            self.send_response(200); self.send_header('Content-Type', 'text/html'); self.send_header('Content-Length', str(len(PAGE))); self.end_headers(); self.wfile.write(PAGE); return
        if self.path.startswith('/redir'):
            self.send_response(302 if '302' in self.path else 307); self.send_header('Location', '/echo'); self.send_header('Content-Length', '0'); self.end_headers(); return
        if self.path.startswith('/echo'):
            if self.command == 'POST' and 'octet' not in (self.headers.get('Content-Type') or '') and len(body) == 1024 and body[:2] == b'\x00\x01':
                self.send_response(200); self.send_header('Content-Type', 'application/octet-stream'); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body); return
            out = json.dumps({'method': self.command, 'ctype': self.headers.get('Content-Type'), 'xtest': self.headers.get('X-Test'),
                              'len': len(body), 'body': body[:60].decode('latin1'), 'host': self.headers.get('Host')}).encode()
            self.send_response(200); self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(out)))
            self.send_header('Access-Control-Allow-Origin', '*'); self.end_headers(); self.wfile.write(out); return
        self.send_response(404); self.send_header('Content-Length', '0'); self.end_headers()
    do_GET = do_POST = do_PUT = do_DELETE = _do

srv = ThreadingHTTPServer(('0.0.0.0', 8003), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
results = []
def step(name, ok):
    results.append((name, bool(ok))); print(('PASS ' if ok else 'FAIL ') + name, flush=True)

tag = sys.argv[1] if len(sys.argv) > 1 else 'okaipost'
vm = OkVM(tag)
try:
    vm.wait_for('[mem] heap', timeout=60)
    time.sleep(10)
    want = 'okai http://10.0.2.2:8003/'
    for attempt in range(4):
        vm.type_string(want + '\n', delay=0.05)
        time.sleep(4)
        if '[okai] fetch http://10.0.2.2:8003/' in vm.serial(): break
        print('  (command garbled, retrying)'); vm.type_string('\n', delay=0.05); time.sleep(2)
    done = vm.wait_for('POSTTEST DONE', timeout=150, fail_pats=['POSTTEST FAILED'])
    log = vm.serial()
    for l in re.findall(r'\[js\] POSTTEST[^\n]*', log): print('   ', l)
    step('json POST: method, content-type, custom header, body', 'POSTTEST json POST application/json one 7 {"a":1}' in log)
    m = re.search(r'POSTTEST binary len=(\d+) sum=(\d+) want=(\d+)', log)
    step('binary body round trip (1024 bytes, all values)', m and m.group(1) == '1024' and m.group(2) == m.group(3))
    step('40 KB body (several TLS-sized chunks)', 'POSTTEST big len=40000 ctype=text/plain;charset=UTF-8' in log)
    step('PUT with body', 'POSTTEST put PUT 5' in log)
    step('DELETE without body', 'POSTTEST delete DELETE 0' in log)
    step('302 on a POST becomes a GET', 'POSTTEST 302 GET 0' in log)
    step('307 on a POST stays a POST with its body', 'POSTTEST 307 POST 3' in log)
    step('XHR POST: own content-type, forbidden Host header ignored', re.search(r'POSTTEST xhr POST text/x-test 5 host=10\.0\.2\.2:8003', log) is not None)
    step('all done', done)
finally:
    vm.kill()
    srv.shutdown()
print('RESULT:', 'PASS' if results and all(r for _, r in results) else 'FAIL')
sys.exit(0 if results and all(r for _, r in results) else 1)
