#!/usr/bin/env python3
"""Long URLs and a big cookie jar, the shape of Google's sign-in redirects (accounts.google.com/CheckCookie?continue=<nested
URLs>, then SetSID, then youtube.com/signin).

A local server (:8007) answers /start with a 302 chain whose Location headers carry 4-6 KB of percent-escaped query (a
nested continue= URL inside a continue=), and sets ~40 cookies of ~150 bytes (6 KB in all) that the final request must
carry back COMPLETE. The URL buffers used to be 2048 bytes (silently cut: a malformed request, Google answers 400) and the
Cookie header was capped near 3.5 KB (the rest silently dropped). PASS: the final request line equals what the server
put in the last Location, byte for byte, and the Cookie header holds all 40 cookies."""
import sys, os, time, re, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import quote
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

NCK = 40
inner = "https://www.youtube.com/signin?action_handle_signin=true&app=desktop&next=" + quote("https://www.youtube.com/watch?v=aqz-KE-bpKQ&t=12s", safe="")
mid = "https://accounts.youtube.com/accounts/SetSID?ssdc=1&sidt=" + "A" * 900 + "&continue=" + quote(inner, safe="")
LOC1 = "/r1?pstMsg=1&checkConnection=youtube%3A" + "9" * 40 + "&checkedDomains=youtube&continue=" + quote(mid, safe="") + "&token=" + "Z" * 1500
LOC2 = "/final?continue=" + quote("http://10.0.2.2:8007" + LOC1, safe="") + "&sig=" + quote("a+b/c=d~e!f*g'h(i)j", safe="~!*'()") + "&pad=" + "x" * 1800
print("Location sizes:", len(LOC1), len(LOC2))
seen = {}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def reply(self, code, body=b"", extra=()):
        self.send_response(code)
        for k, v in extra:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/start":
            ck = [("Set-Cookie", "ck%02d=%s; Path=/; Max-Age=3600" % (i, chr(65 + i % 26) * 140)) for i in range(NCK)]
            self.reply(302, extra=ck + [("Location", LOC1)])
        elif self.path.startswith("/r1"):
            seen["r1"] = self.path
            self.reply(302, extra=[("Location", LOC2)])
        elif self.path.startswith("/final"):
            seen["final"] = self.path
            seen["cookie"] = self.headers.get("Cookie", "")
            self.reply(200, b"<html><body><h1>LONGURL-DONE</h1></body></html>", [("Content-Type", "text/html")])
        else:
            self.reply(404)


srv = ThreadingHTTPServer(("0.0.0.0", 8007), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
results = []


def step(name, ok, extra=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + ((" " + extra) if extra else ""), flush=True)


vm = None
try:
    vm = OkVM("okailongurl")
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("okai http://10.0.2.2:8007/start\n", delay=0.05)
    for _ in range(60):
        if "final" in seen:
            break
        time.sleep(1)
    step("redirect chain reached /final", "final" in seen)
    step("middle hop request line intact", seen.get("r1") == LOC1, "(%d vs %d)" % (len(seen.get("r1", "")), len(LOC1)))
    step("final request line intact", seen.get("final") == LOC2, "(%d vs %d)" % (len(seen.get("final", "")), len(LOC2)))
    got = seen.get("cookie", "")
    names = re.findall(r"ck(\d\d)=", got)
    step("all %d cookies sent (%d bytes)" % (NCK, len(got)), len(set(names)) == NCK)
    log = vm.serial()
    step("no 'Location cut' lines", "Location of" not in log)
    bad = [l for l in log.splitlines() if "kmalloc FAIL" in l or "bad or double free" in l or "PAGE FAULT" in l.upper()]
    step("no faults", not bad, str(bad[:3]))
    print("RESULT:", "PASS" if all(results) else "FAIL")
finally:
    if vm:
        vm.kill()
    srv.shutdown()
sys.exit(0 if all(results) else 1)
