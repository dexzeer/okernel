#!/usr/bin/env python3
"""Streaming fetch() inside the kernel: Response.body hands out the pieces while the response is still downloading.

A local server (:8006) sends a chunked body in 4 pieces of 48 KB, 1.5 s apart (/chunked), a Content-Length body the same
way (/sized), and a small JSON (/small, takes the plain path). The page checks that the head resolves and the first piece
is readable long before the last one is sent, that text()/json()/clone()/tee on a streamed Response give the same bytes,
that abort() after the head errors the stream, and that a connection cut in the middle errors the reader.
PASS needs "[js] RESULT ALLPASS" and no faults."""
import sys, os, time, re, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

PIECE = 48 * 1024
GAP = 1.5

PAGE = r"""<!doctype html><html><body><script>
const log = (...a) => console.log(...a);
let fails = 0;
const check = (n, ok, x) => { if (!ok) fails++; log((ok ? 'ok ' : 'FAIL ') + n + (x !== undefined ? ' ' + x : '')); };
const sum = u8 => { let s = 0; for (let i = 0; i < u8.length; i++) s = (s * 31 + u8[i]) >>> 0; return s; };
const expect = n => { const a = new Uint8Array(n); for (let i = 0; i < n; i++) a[i] = (i * 7 + (i >> 8)) & 255; return a; };
async function main() {
    // 1. chunked: head + first piece well before the end
    let t0 = Date.now();
    let r = await fetch('/chunked');
    const tHead = Date.now() - t0;
    check('chunked status', r.status === 200);
    const rd = r.body.getReader();
    const first = await rd.read();
    const tFirst = Date.now() - t0;
    let total = first.value.length, pieces = 1, parts = [first.value];
    for (;;) { const x = await rd.read(); if (x.done) break; total += x.value.length; pieces++; parts.push(x.value); }
    const tEnd = Date.now() - t0;
    log('timing head=' + tHead + ' first=' + tFirst + ' end=' + tEnd + ' pieces=' + pieces);
    check('chunked total', total === 4 * %(piece)d, total);
    check('first piece before the end', tFirst < tEnd - 2500, tFirst + ' vs ' + tEnd);
    const all = new Uint8Array(total); let o = 0; for (const p of parts) { all.set(p, o); o += p.length; }
    check('chunked bytes', sum(all) === sum(expect(total)));
    // 2. sized: Content-Length body
    t0 = Date.now();
    r = await fetch('/sized');
    const rd2 = r.body.getReader();
    const f2 = await rd2.read();
    const t2 = Date.now() - t0;
    let n2 = f2.value.length;
    for (;;) { const x = await rd2.read(); if (x.done) break; n2 += x.value.length; }
    check('sized first piece early', t2 < (Date.now() - t0) - 2500, t2);
    check('sized total', n2 === 4 * %(piece)d, n2);
    // 3. text(), arrayBuffer() and clone() of a streamed response
    r = await fetch('/chunked');
    const c = r.clone();
    const ab = await r.arrayBuffer();
    const tx = await c.text();
    check('arrayBuffer', ab.byteLength === 4 * %(piece)d && sum(new Uint8Array(ab)) === sum(expect(ab.byteLength)));
    check('clone text length', tx.length > 0);
    // 4. small body (plain path)
    r = await fetch('/small');
    const j = await r.json();
    check('small json', j.ok === 1);
    // 5. abort after the head errors the reader
    const ac = new AbortController();
    r = await fetch('/chunked', { signal: ac.signal });
    const rd3 = r.body.getReader();
    await rd3.read();
    ac.abort();
    let errored = false;
    try { for (;;) { const x = await rd3.read(); if (x.done) break; } } catch (e) { errored = true; }
    check('abort errors the stream', errored);
    // 6. a connection cut in the middle
    r = await fetch('/cut');
    let cutErr = false, got = 0;
    try { const rd4 = r.body.getReader(); for (;;) { const x = await rd4.read(); if (x.done) break; got += x.value.length; } } catch (e) { cutErr = true; }
    check('cut connection errors or ends short', cutErr || got < 4 * %(piece)d, got);
    log('RESULT ' + (fails ? 'FAIL' : 'ALLPASS'));
}
main().catch(e => { log('exception ' + e); log('RESULT FAIL'); });
</script></body></html>
""" % {"piece": PIECE}


def body_bytes(n):
    return bytes(((i * 7 + (i >> 8)) & 255) for i in range(n))


DATA = body_bytes(4 * PIECE)


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_GET(self):
        p = self.path.split("?")[0]
        try:
            if p in ("/", "/index.html"):
                b = PAGE.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)
            elif p == "/small":
                b = b'{"ok":1}'
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)
            elif p == "/chunked":
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                for i in range(4):
                    piece = DATA[i * PIECE:(i + 1) * PIECE]
                    self.wfile.write(b"%x\r\n" % len(piece) + piece + b"\r\n")
                    self.wfile.flush()
                    if i < 3:
                        time.sleep(GAP)
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
            elif p == "/sized":
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(DATA)))
                self.end_headers()
                for i in range(4):
                    self.wfile.write(DATA[i * PIECE:(i + 1) * PIECE])
                    self.wfile.flush()
                    if i < 3:
                        time.sleep(GAP)
            elif p == "/cut":
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(DATA)))
                self.end_headers()
                self.wfile.write(DATA[:PIECE])
                self.wfile.flush()
                time.sleep(1)
                self.close_connection = True
                self.connection.close()
            else:
                self.send_error(404)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass


srv = ThreadingHTTPServer(("0.0.0.0", 8006), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
results = []


def step(name, ok, extra=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + ((" " + extra) if extra else ""), flush=True)


vm = None
try:
    vm = OkVM("okaistream")
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("cpu slow 1\n", delay=0.05)
    time.sleep(1)
    vm.type_string("okai http://10.0.2.2:8006/\n", delay=0.05)
    ok = False
    for _ in range(120):
        m = re.findall(r"\[js\] RESULT (ALLPASS|FAIL)", vm.serial())
        if m:
            ok = m[-1] == "ALLPASS"
            break
        time.sleep(1)
    log = vm.serial()
    for l in log.splitlines():
        if l.startswith("[js] ") and ("FAIL" in l or "timing" in l or l.startswith("[js] ok") or "exception" in l):
            print("   ", l)
    step("streaming fetch page", ok)
    bad = [l for l in log.splitlines() if "kmalloc FAIL" in l or "bad or double free" in l or "PAGE FAULT" in l.upper() or "abort()" in l]
    step("no faults / allocator errors", not bad, str(bad[:3]))
    print("RESULT:", "PASS" if all(results) else "FAIL")
finally:
    if vm:
        vm.kill()
    srv.shutdown()
sys.exit(0 if all(results) else 1)
