#!/usr/bin/env python3
# Negative test: self-signed HTTPS server (not in root store) must be
# REJECTED with the SECURITY WARNING page and NO plain-HTTP fallback.
import sys, time, os, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM, OUTDIR

# Self-contained: a fresh self-signed certificate (never in the root store)
# served by `openssl s_server` on the host's :8443 (guest reaches 10.0.2.2).
CERT = os.path.join(OUTDIR, "certfail_cert.pem")
KEY = os.path.join(OUTDIR, "certfail_key.pem")
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", KEY,
                "-out", CERT, "-days", "2", "-subj", "/CN=10.0.2.2"],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
srv = subprocess.Popen(["openssl", "s_server", "-accept", "8443", "-cert", CERT, "-key", KEY,
                        "-www", "-tls1_3"],
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)

vm = OkVM("certfail")
time.sleep(14)
# HMP sendkey timing flakes on shift-punctuation (observed: ":8443" lost
# when shift-semicolon mistimes — same class as the ithub.com focus flake).
# Verify the URL arrived intact, retype if mangled (max 3 tries).
typed_ok = False
for _ in range(3):
    vm.type_string("okai https://10.0.2.2:8443/\n")
    time.sleep(6)
    if "10.0.2.2:8443" in vm.serial():
        typed_ok = True
        break
    print("retyping URL (sendkey flake)...")
print("url typed:", "PASS" if typed_ok else "FAIL")
vm.wait_for("handshake/download FAILED", timeout=90)
s = vm.serial()
ok = True
def check(name, cond):
    global ok
    print(f"{name}: {'PASS' if cond else 'FAIL'}")
    ok = ok and cond

check("url typed", typed_ok)
ok = ok and typed_ok

check("TLS failed", "handshake/download FAILED" in s)
check("no HTTP fallback", "http fallback" not in s)
check("chain verified NOT printed", "certificate chain verified" not in s)
# failure must be a CERT-class reason (4=cert, 5=hostname, 6=proto)
import re
m = re.search(r"FAILED \(reason=(\d+)", s)
reason = int(m.group(1)) if m else -1
check(f"reason is cert-class (got {reason})", reason in (4, 5, 6))
vm.dump()
vm.kill()
srv.terminate()
print("CERTFAIL-QEMU:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
