#!/usr/bin/env python3
# Generate a preload-pin table entry: fetches the live chain and prints the C
# snippet with the SPKI hash of the certificate after the leaf, the issuing CA.
# Hand-typing 32-byte hashes caused a wrong-pin incident (bisected 2026-09-12):
# always generate. Run it a few times for a CDN: a host must serve ONE issuer.
# Usage: python3 tools/gen_preload.py <host>
# VERIFY with a live fetch (build-host/tls_scan <host>) before committing the entry.
import re, subprocess, sys
h = sys.argv[1]
out = subprocess.run(["openssl", "s_client", "-showcerts", "-connect", f"{h}:443", "-servername", h],
                     input=b"", capture_output=True, timeout=20).stdout
certs = re.findall(rb"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", out, re.S)
if len(certs) < 2:
    sys.exit("the server sent no issuer certificate")
name = subprocess.run(["openssl", "x509", "-noout", "-subject"], input=certs[1], capture_output=True).stdout.decode().strip()
pem = subprocess.run(["openssl", "x509", "-noout", "-pubkey"], input=certs[1], capture_output=True).stdout
raw = subprocess.run(["openssl", "pkey", "-pubin", "-outform", "DER"], input=pem, capture_output=True).stdout
dgst = subprocess.run(["openssl", "dgst", "-sha256"], input=raw, capture_output=True).stdout.decode()
hx = dgst.strip().split("= ")[1].replace(" ", "")
bs = ["0x" + hx[i:i + 2] for i in range(0, 64, 2)]
print("    // %s: %s" % (h, name))
print('    { "%s", { %s,' % (h, ",".join(bs[:16])))
print('                      %s } },' % ",".join(bs[16:]))
