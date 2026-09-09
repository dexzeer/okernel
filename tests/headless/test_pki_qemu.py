#!/usr/bin/env python3
# QEMU verification: full PKI in the kernel — example.com over HTTPS with
# certificate chain verification, root anchoring, hostname match, and
# CertificateVerify signature check.
import sys, time
sys.path.insert(0, 'tests/headless')
from okvm import OkVM

vm = OkVM("pki")
time.sleep(14)
ok = True

# Wall clock (RTC) present?
early = vm.serial()
if "[rtc] wall clock:" in early:
    line = [l for l in early.splitlines() if "[rtc] wall clock:" in l][-1]
    print("RTC:", line.strip())
else:
    print("RTC: NO wall-clock line (default date used) — check [rtc] lines:")
    for l in early.splitlines():
        if "[rtc]" in l: print("   ", l.strip())
    ok = False

vm.type_string("okai https://example.com/\n")
r = vm.wait_for("https parse: count=", timeout=90)
cert_ok = "certificate chain verified, host matched" in vm.serial()
cv_ok = "CertificateVerify signature INVALID" not in vm.serial()
print("https parse:", "PASS" if r else "FAIL")
print("chain verified:", "PASS" if cert_ok else "FAIL")
print("no CV invalid:", "PASS" if cv_ok else "FAIL")
ok = ok and r and cert_ok and cv_ok
vm.dump()
vm.kill()
print("PKI-QEMU:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
