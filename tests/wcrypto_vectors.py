#!/usr/bin/env python3
"""Test vectors for src/rtw/wcrypto.c from independent implementations (Python hashlib / hmac / cryptography)."""
import hashlib, hmac, os, random, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.keywrap import aes_key_wrap

random.seed(int(sys.argv[1]) if len(sys.argv) > 1 else 1)
rb = lambda n: bytes(random.getrandbits(8) for _ in range(n))
h = lambda b: b.hex() if b else "-"

def prf(key, label, data, outlen):
    out = b""
    i = 0
    while len(out) < outlen:
        out += hmac.new(key, label.encode() + b"\0" + data + bytes([i]), hashlib.sha1).digest()
        i += 1
    return out[:outlen]

# fixed, published vectors
print("psk password IEEE f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e")
print("pbkdf2", h(b"password"), h(b"salt"), 4096, 20, "4b007901b765489abead49d926f721d065a429c1")
kek = bytes(range(16)); data = bytes.fromhex("00112233445566778899AABBCCDDEEFF")
print("unwrap", h(kek), "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", h(data))

for _ in range(40):
    k = rb(random.randint(1, 100)); m = rb(random.randint(0, 200))
    print("hmac", h(k), h(m), hmac.new(k, m, hashlib.sha1).hexdigest())
for _ in range(6):
    p = rb(random.randint(8, 63)); s = rb(random.randint(1, 32)); it = random.choice([1, 2, 100, 4096]); n = random.choice([20, 32, 48])
    print("pbkdf2", h(p), h(s), it, n, hashlib.pbkdf2_hmac("sha1", p, s, it, n).hex())
for _ in range(10):
    k = rb(32); d = rb(random.randint(1, 120)); n = random.choice([16, 48, 64])
    print("prf", h(k), "Pairwise_key_expansion", h(d), n, prf(k, "Pairwise key expansion", d, n).hex())
for _ in range(20):
    k = rb(16); pt = rb(16)
    c = Cipher(algorithms.AES(k), modes.ECB()).encryptor(); ct = c.update(pt) + c.finalize()
    print("dec", h(k), h(ct), h(pt))
for _ in range(20):
    k = rb(16); n8 = random.randint(2, 8); pt = rb(8 * n8)
    print("unwrap", h(k), aes_key_wrap(k, pt).hex(), h(pt))
for _ in range(10):
    pmk = rb(32); aa = rb(6); spa = rb(6); an = rb(32); sn = rb(32)
    data = min(aa, spa) + max(aa, spa) + min(an, sn) + max(an, sn)
    print("ptk", h(pmk), h(aa), h(spa), h(an), h(sn), prf(pmk, "Pairwise key expansion", data, 48).hex())
for _ in range(10):
    k = rb(16); n8 = random.randint(2, 6); pt = rb(8 * n8)
    print("wrap", h(k), h(pt), aes_key_wrap(k, pt).hex())
for _ in range(40):
    k = rb(16); nonce = rb(13); aad = rb(random.randint(0, 40)); pt = rb(random.randint(0, 300))
    out = AESCCM(k, tag_length=8).encrypt(nonce, pt, aad if aad else None)
    print("ccm", h(k), h(nonce), h(aad), h(pt), h(out[:-8]), h(out[-8:]))
