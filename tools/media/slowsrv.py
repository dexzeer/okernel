import http.server, socketserver, time, os, re
DELAY = float(os.environ.get("DELAY", "3"))
class H(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.endswith(".webm"):
            time.sleep(DELAY)
        return super().do_GET()
    def log_message(self, *a): pass
class T(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
T(("0.0.0.0", 8006), H).serve_forever()
