#!/usr/bin/env python3
"""Verify ALL okai navigations default to HTTPS.

Boots the rebuilt kernel, opens okai with a *bare* host (`okai example.com`),
and checks the serial ground-truth: the page must be fetched + parsed over
HTTPS (the desktop TLS branch prints `[br] https parse: count=` — this proves
the bare host was upgraded to https:// and actually loaded over TLS). It must
NOT have fallen back to plain HTTP (no `[br] http parse` / no `https failed`).
This proves the http->https default upgrade for the most common user action
(typing a bare website).
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from okvm import OkVM

vm = OkVM("httpsdefault")
time.sleep(14)
print("booted; typing okai example.com", flush=True)
vm.type_string("okai http://example.com/\n")

# 1) The page must be fetched + parsed over HTTPS (desktop TLS branch prints
#    "[br] https parse: count="). Proves the bare host was upgraded to
#    https:// and actually loaded over TLS.
ok_https = vm.wait_for("[br] https parse", timeout=90)
print("https-parse-marker:", "FOUND" if ok_https else "MISSING", flush=True)

# 2) It must NOT have fallen back to plain HTTP.
log = vm.serial()
fell_back = ("https failed" in log) or ("[br] http parse" in log)
print("http-fallback:", "TRIGGERED(bad)" if fell_back else "none(good)", flush=True)

# Relevant serial lines for the record.
for line in log.splitlines():
    if "https parse" in line or "http parse" in line or "https failed" in line:
        print("  >>", line, flush=True)

ok = ok_https and not fell_back
print("HTTPS_DEFAULT_TEST", "PASS" if ok else "FAIL")
vm.kill()
sys.exit(0 if ok else 1)
