#!/usr/bin/env python3
# Random AES-128-GCM vectors from OpenSSL (python3-cryptography) for
# tests/test_aes.c: "key iv aad pt ct tag" per line, hex ('-' = empty).
import os, sys
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

n = int(sys.argv[1]) if len(sys.argv) > 1 else 400
lens = [0, 1, 15, 16, 17, 31, 32, 33, 47, 48, 64, 100, 255, 1000, 4096, 16383]
for i in range(n):
    key, iv = os.urandom(16), os.urandom(12)
    aad = os.urandom([0, 5, 13, 16, 20, 300][i % 6])
    pt = os.urandom(lens[i % len(lens)])
    out = AESGCM(key).encrypt(iv, pt, aad)
    ct, tag = out[:-16], out[-16:]
    h = lambda b: b.hex() if b else "-"
    print(h(key), h(iv), h(aad), h(pt), h(ct), h(tag))
