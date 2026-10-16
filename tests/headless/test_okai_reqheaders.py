#!/usr/bin/env python3
"""The request headers okai really sends (a local server records them; ports 8008 and 8009 are two different origins).

What a browser sends and the kernel used to omit: the User-Agent on the wire must equal navigator.userAgent (it was
"okernel/0.4" while scripts saw another string), Accept-Language must match navigator.languages (it was absent), sub-resource
requests carry a Referer (full URL to the same origin, the origin alone cross-origin), a click on a cross-origin link carries the
origin as Referer and Upgrade-Insecure-Requests: 1, Origin goes out with every unsafe method (POST) and with cross-origin CORS
requests, and an address-bar navigation carries no Referer. Sec-Fetch-* is only sent to https / loopback targets, so none
must appear here (checked: absent over plain http to 10.0.2.2).
Never sent, by design: client hints (Sec-CH-UA*) and any other brand assertion."""
import sys, os, time, re, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

seen = {}      # (port, path) -> dict of lowercased headers (first request for that path)
ua_js = []


def make_handler(port):
    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        def rec(self):
            seen.setdefault((port, self.path.split("?")[0]), {k.lower(): v for k, v in self.headers.items()})

        def reply(self, body, ctype, extra=()):
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in extra:
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            self.rec()
            p = self.path.split("?")[0]
            cors = [("Access-Control-Allow-Origin", "*")]
            if p == "/":
                page = b"""<!doctype html><html><head><title>hdr</title><link rel="stylesheet" href="/s.css"></head><body>
<h1>headers</h1><img src="/i.png" width="4" height="4"><div style="margin:60px 0 0 500px"><a href="http://10.0.2.2:8009/next">go next</a></div>
<script src="/a.js"></script></body></html>"""
                self.reply(page, "text/html; charset=utf-8")
            elif p == "/s.css":
                self.reply(b"h1{color:#036}", "text/css")
            elif p == "/a.js":
                js = b"""console.log('UA ' + navigator.userAgent);
console.log('LANGS ' + navigator.languages.join(','));
console.log('APPVERSION ' + navigator.appVersion);
fetch('/api/get').then(function (r) { return r.text(); });
var x = new XMLHttpRequest(); x.open('POST', '/api/post'); x.setRequestHeader('Content-Type', 'text/plain'); x.send('hello');
fetch('http://10.0.2.2:8009/cross').then(function (r) { return r.text(); }).then(function () { console.log('CROSS-DONE'); });
"""
                self.reply(js, "application/javascript")
            elif p == "/i.png":
                import base64
                self.reply(base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAYAAACp8Z5+AAAAEklEQVR4nGNgYGD4z8DAwMAAAA0AAv+0Zz1VAAAAAElFTkSuQmCC"), "image/png")
            elif p == "/cross":
                self.reply(b"ok", "text/plain", cors)
            elif p == "/next":
                self.reply(b"<html><body><h1>next page</h1></body></html>", "text/html")
            else:
                self.reply(b"ok", "text/plain")

        def do_POST(self):
            n = int(self.headers.get("Content-Length", "0") or 0)
            self.rfile.read(n)
            self.rec()
            self.reply(b"posted", "text/plain")
    return H


servers = []
for port in (8008, 8009):
    s = ThreadingHTTPServer(("0.0.0.0", port), make_handler(port))
    threading.Thread(target=s.serve_forever, daemon=True).start()
    servers.append(s)
results = []


def step(name, ok, extra=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + ((" " + extra) if extra else ""), flush=True)


vm = None
try:
    vm = OkVM("okaireqhdr")
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(8)
    vm.type_string("cpu slow 1\n", delay=0.05)
    time.sleep(1)
    vm.type_string("okai http://10.0.2.2:8008/\n", delay=0.05)
    for _ in range(60):
        if (8009, "/cross") in seen and (8008, "/api/post") in seen and (8008, "/i.png") in seen:
            break
        time.sleep(1)
    time.sleep(2)
    log = vm.serial()
    m = re.search(r"\[js\] UA (.+)", log)
    ua_js = m.group(1).strip() if m else ""
    root = seen.get((8008, "/"), {})
    step("page request reached the server", bool(root))
    step("User-Agent on the wire == navigator.userAgent", root.get("user-agent") == ua_js and ua_js != "", "(%r vs %r)" % (root.get("user-agent"), ua_js))
    step("User-Agent claims no Chrome/Firefox", ua_js and "Chrome" not in ua_js and "Firefox" not in ua_js)
    m2 = re.search(r"\[js\] LANGS (.+)", log)
    langs = m2.group(1).strip() if m2 else ""
    step("Accept-Language is sent and starts with navigator.languages[0]", root.get("accept-language", "").startswith(langs.split(",")[0]) and langs != "", "(%r vs %r)" % (root.get("accept-language"), langs))
    m3 = re.search(r"\[js\] APPVERSION (.+)", log)
    step("navigator.appVersion is the user agent without 'Mozilla/'", m3 and ua_js.replace("Mozilla/", "", 1) == m3.group(1).strip())
    step("address-bar navigation: no Referer", "referer" not in root)
    step("document request: Upgrade-Insecure-Requests: 1", root.get("upgrade-insecure-requests") == "1")
    css = seen.get((8008, "/s.css"), {})
    step("stylesheet: Referer is the page URL", css.get("referer") == "http://10.0.2.2:8008/", repr(css.get("referer")))
    scr = seen.get((8008, "/a.js"), {})
    step("script: Referer is the page URL", scr.get("referer") == "http://10.0.2.2:8008/", repr(scr.get("referer")))
    img = seen.get((8008, "/i.png"), {})
    step("image: Referer is the page URL", img.get("referer") == "http://10.0.2.2:8008/", repr(img.get("referer")))
    get = seen.get((8008, "/api/get"), {})
    step("same-origin fetch GET: Referer, no Origin", get.get("referer") == "http://10.0.2.2:8008/" and "origin" not in get, repr((get.get("referer"), get.get("origin"))))
    post = seen.get((8008, "/api/post"), {})
    step("same-origin XHR POST: Origin sent", post.get("origin") == "http://10.0.2.2:8008", repr(post.get("origin")))
    cross = seen.get((8009, "/cross"), {})
    step("cross-origin fetch: Origin sent, Referer is the origin only", cross.get("origin") == "http://10.0.2.2:8008" and cross.get("referer") == "http://10.0.2.2:8008/", repr((cross.get("origin"), cross.get("referer"))))
    sec = [k for d in seen.values() for k in d if k.startswith("sec-fetch") or k.startswith("sec-ch-ua") or k.startswith("x-browser")]
    step("no Sec-Fetch-*/client hints over plain http (and never client hints)", not sec, str(sec[:4]))
    # click the cross-origin link
    ok = vm.click_link("8009/next")
    for _ in range(40):
        if (8009, "/next") in seen:
            break
        time.sleep(1)
    nxt = seen.get((8009, "/next"), {})
    step("link click reached the other origin", bool(nxt))
    step("link click: Referer is the origin of the page it was on", nxt.get("referer") == "http://10.0.2.2:8008/", repr(nxt.get("referer")))
    step("link click: Upgrade-Insecure-Requests: 1", nxt.get("upgrade-insecure-requests") == "1")
    bad = [l for l in vm.serial().splitlines() if "kmalloc FAIL" in l or "bad or double free" in l or "PAGE FAULT" in l.upper()]
    step("no faults", not bad, str(bad[:3]))
    print("RESULT:", "PASS" if all(results) else "FAIL")
finally:
    if vm:
        vm.kill()
    for s in servers:
        s.shutdown()
sys.exit(0 if all(results) else 1)
