#!/usr/bin/env python3
# Generate a preload-pin table entry (cryptoholes follow-up): fetches the
# live leaf SPKI hash AND prints the C snippet. Hand-typing 32-byte hashes
# caused a wrong-pin incident (bisected 2026-09-12) — always generate.
# Usage: python3 tools_gen_preload.py <host>
# VERIFY with a live fetch (t_tlsa) before committing the entry.
import subprocess, sys
h = sys.argv[1]
der = subprocess.run(["openssl","s_client","-connect",f"{h}:443",
                      "-servername",h], input=b"",
                     capture_output=True, timeout=20).stdout
pem = subprocess.run(["openssl","x509","-noout","-pubkey"],
                     input=der, capture_output=True).stdout
raw = subprocess.run(["openssl","pkey","-pubin","-outform","DER"],
                     input=pem, capture_output=True).stdout
dgst = subprocess.run(["openssl","dgst","-sha256"],
                      input=raw, capture_output=True).stdout.decode()
hx = dgst.strip().split("= ")[1].replace(" ", "")
bs = ["0x"+hx[i:i+2] for i in range(0, 64, 2)]
print('    { "%s", { %s,' % (h, ",".join(bs[:16])))
print('                      %s } },' % ",".join(bs[16:]))
