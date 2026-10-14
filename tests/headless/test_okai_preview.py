#!/usr/bin/env python3
"""okai preview of a slowly downloading document (offline, local server on :8007).

The document is 40 KB of paragraphs sent in two parts with a 3 s pause in between. After 0.6 s with 20 KB in hand okai shows
what it has (scripts off); when the rest arrives the real load replaces it, so the final page must contain the last paragraph
and every paragraph must have been fetched exactly once from the server's point of view (one GET for the document).
"""
import sys, os, time, re, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

N = 1200
HEAD = '<!doctype html><html><head><title>preview</title></head><body>\n' + ''.join('<p>paragraph number %d of the long page</p>\n' % i for i in range(N // 2))
TAIL = ''.join('<p>paragraph number %d of the long page</p>\n' % i for i in range(N // 2, N)) + '<p id=last>THE END</p></body></html>'
FULL = (HEAD + TAIL).encode()
gets = [0]
sent_tail_at = [0.0]

class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path != '/':
            self.send_response(404); self.send_header('Content-Length', '0'); self.end_headers(); return
        gets[0] += 1
        cut = len(HEAD.encode())
        self.send_response(200); self.send_header('Content-Type', 'text/html'); self.send_header('Content-Length', str(len(FULL)))
        self.send_header('Cache-Control', 'no-store'); self.end_headers()
        self.wfile.write(FULL[:cut]); self.wfile.flush()
        time.sleep(3.0)
        sent_tail_at[0] = time.time()
        self.wfile.write(FULL[cut:]); self.wfile.flush()

srv = ThreadingHTTPServer(('0.0.0.0', 8007), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
results = []
def step(name, ok):
    results.append((name, bool(ok))); print(('PASS ' if ok else 'FAIL ') + name, flush=True)

vm = OkVM(sys.argv[1] if len(sys.argv) > 1 else 'okaipreview')
try:
    vm.wait_for('[mem] heap', timeout=60)
    time.sleep(10)
    want = 'okai http://10.0.2.2:8007/'
    for attempt in range(4):
        vm.type_string(want + '\n', delay=0.05)
        time.sleep(4)
        if '[okai] fetch http://10.0.2.2:8007/' in vm.serial(): break
        print('  (command garbled, retrying)'); vm.type_string('\n', delay=0.05); time.sleep(2)
    vm.wait_for('sub-res done', timeout=60)
    time.sleep(6)
    log = vm.serial()
    step('a preview was built while the document was still downloading', '[okai] preview of' in log and log.index('[okai] preview of') < log.index('parse: count='))
    counts = [int(x) for x in re.findall(r'parse: count=(\d+)', log)]
    nodes = [int(x) for x in re.findall(r'dom_nodes=(\d+)', log)]
    pv = re.search(r'preview of (\d+) bytes', log)
    step('the preview was a partial document (20 KB to 30 KB)', pv and 20000 <= int(pv.group(1)) <= 30000)
    step('the document was requested once', gets[0] == 1)
    step('the final DOM holds the last paragraph (about %d nodes)' % (N * 2 + 6), nodes and nodes[-1] >= N * 2)
    step('no faults', not re.search(r'PAGE FAULT|\[ISR\] Exception|kmalloc FAIL', log))
    print('  parse counts', counts)
finally:
    vm.kill()
    srv.shutdown()
print('RESULT:', 'PASS' if results and all(r for _, r in results) else 'FAIL')
sys.exit(0 if results and all(r for _, r in results) else 1)
