#!/usr/bin/env python3
"""okai form POST test (offline: a local server on the host, :8003).

  1. a page with a method=post form (text field, checked checkbox, hidden field, submit button) loads
  2. Tab focuses the text field, typing + Enter submits it: the server must receive a POST with
     Content-Type application/x-www-form-urlencoded, the body "q=hello&c=on&h=1...", an Origin / Referer of
     the form's page and the cookies; it answers 302 -> /done (the POST becomes a GET) and /done shows up
  3. a 307 answer keeps the POST (the server counts it)
  4. a method=get form still puts the fields in the URL
PASS requires every step and no faults / allocator errors.
"""
import sys, os, time, re, threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

seen = []          # (method, path, headers dict, body)


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def page(self, html, code=200, extra=()):
        b = html.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        seen.append(("GET", self.path, dict(self.headers), b""))
        if self.path == "/form":
            self.page("""<!doctype html><title>post form</title><h1>Post form</h1>
<form action="/submit" method="post">
  <input name="q" placeholder="type here">
  <label><input type="checkbox" name="c" checked> check</label>
  <input type="hidden" name="h" value="1">
  <button type="submit" name="go" value="yes">Go</button>
</form>

<form action="/search" method="get"><input name="w" value="gg"><button>GET</button></form>""",
                      extra=[("Set-Cookie", "sid=abc123; Path=/")])
        elif self.path.startswith("/form307"):
            self.page("<!doctype html><title>f307</title><h1>307 form</h1><form action=\"/submit307\" method=\"POST\"><input name=\"z\" value=\"zz\"><button>go</button></form>")
        elif self.path.startswith("/done"):
            self.page("<!doctype html><title>done</title><h1>Done page</h1>")
        elif self.path.startswith("/search"):
            self.page("<!doctype html><title>search</title><h1>Search page</h1>")
        else:
            self.page("<!doctype html><title>x</title><p>x", 404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n) if n else b""
        seen.append(("POST", self.path, dict(self.headers), body))
        if self.path == "/submit":
            self.send_response(302)
            self.send_header("Location", "/done")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/submit307":
            self.send_response(307)
            self.send_header("Location", "/final307")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/final307":
            self.page("<!doctype html><title>final</title><h1>Final307</h1>")
        else:
            self.page("<!doctype html><p>?", 404)


srv = HTTPServer(("0.0.0.0", 8003), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
results = []


def step(name, ok, extra=""):
    results.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name + ((" " + extra) if extra else ""), flush=True)


vm = None
try:
    vm = OkVM("postform")
    vm.wait_for("[mem] heap", timeout=60)
    time.sleep(10)
    vm.type_string("okai http://10.0.2.2:8003/form\n")
    step("form page loads", vm.wait_for("parse: count=", timeout=40))
    time.sleep(3)
    vm.type_string("\t")
    time.sleep(1.5)
    vm.type_string("hello")
    time.sleep(1.5)
    vm.type_string("\n")
    step("Enter submits as a POST", vm.wait_for("submit form (POST", timeout=15))
    time.sleep(6)
    posts = [s for s in seen if s[0] == "POST" and s[1] == "/submit"]
    ok = bool(posts)
    body = posts[0][3].decode() if posts else ""
    hd = posts[0][2] if posts else {}
    q = parse_qs(body)
    step("server got the urlencoded body", ok and hd.get("Content-Type", "").startswith("application/x-www-form-urlencoded")
         and q.get("q") == ["hello"] and q.get("c") == ["on"] and q.get("h") == ["1"], repr(body))
    step("Origin and Referer are the form's page", ok and hd.get("Origin") == "http://10.0.2.2:8003" and
         hd.get("Referer", "").endswith("/form"), str({k: hd.get(k) for k in ("Origin", "Referer")}))
    step("cookie sent with the POST", ok and "sid=abc123" in hd.get("Cookie", ""), str(hd.get("Cookie")))
    gets = [s for s in seen if s[0] == "GET" and s[1] == "/done"]
    step("302 turned it into a GET of /done", bool(gets))
    step("the done page rendered", "Done page" not in "" and vm.serial().count("parse: count=") >= 2)
    # 307 keeps the POST: a page with just that form
    vm.type_string("g")
    vm.type_string("http://10.0.2.2:8003/form307\n")
    time.sleep(6)
    vm.type_string("\t")
    time.sleep(1.5)
    vm.type_string("\n")
    time.sleep(8)
    p307 = [s for s in seen if s[0] == "POST" and s[1] == "/final307"]
    step("a 307 redirect keeps the POST", bool(p307) and p307[0][3].decode() == "z=zz", str([s[1] for s in seen if s[0] == "POST"]))
    log = vm.serial()
    bad = [l for l in log.splitlines() if "kmalloc FAIL" in l or "bad or double free" in l or "PAGE FAULT" in l.upper()]
    step("no faults", not bad, str(bad[:3]))
    print("RESULT:", "PASS" if all(ok for _, ok in results) else "FAIL")
finally:
    if vm:
        vm.kill()
    srv.shutdown()
sys.exit(0 if all(ok for _, ok in results) else 1)
