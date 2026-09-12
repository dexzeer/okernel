#!/usr/bin/env python3
"""Independent differential test: okernel crypto vs Python `cryptography`.

Drives tests/differential/diff_oracle.c (the kernel's primitives, built
for host) with random inputs and compares every output against the
`cryptography` package (OpenSSL/BoringSSL-backed — fully independent of
our C code). Any mismatch fails loudly.

Covers (cryptoholes round 4, P0/P2):
  X25519 (incl. low-order u=0/u=1 agreement), SHA-256, HMAC-SHA256,
  HKDF-SHA256 (extract+expand), ChaCha20-Poly1305 (enc+dec, incl. empty
  aad/pt edge cases and tag-tamper rejection agreement).

Usage:
  make diff-oracle            # build build-host/diff_oracle
  python3 tests/differential/test_crypto_diff.py [N]   # default N=200/case
"""
import os
import random
import subprocess
import sys

ORACLE = os.path.join(os.path.dirname(__file__), "../../build-host/diff_oracle")

from cryptography.hazmat.primitives import hashes, hmac as hmac_mod
from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey, X25519PublicKey,
)
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF


def hx(b: bytes) -> str:
    return b.hex() if b else "-"


class Oracle:
    def __init__(self):
        self.p = subprocess.Popen(
            [ORACLE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
            bufsize=1,
        )

    def q(self, cmd: str):
        self.p.stdin.write(cmd + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().strip().split(" ")

    def close(self):
        self.p.stdin.close()
        self.p.wait()


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    rng = random.Random(0xC0FFEE)
    o = Oracle()
    fails = 0

    def check(name, got, want):
        nonlocal fails
        if got != want:
            fails += 1
            print(f"MISMATCH {name}: oracle={got} ref={want}")
            if fails > 5:
                print("too many mismatches, aborting")
                o.close()
                sys.exit(1)

    # ---- X25519 (both directions, fresh keys each round) ----
    for i in range(n):
        a = X25519PrivateKey.generate()
        b = X25519PrivateKey.generate()
        sa, pa = a.private_bytes_raw(), a.public_key().public_bytes_raw()
        sb, pb = b.private_bytes_raw(), b.public_key().public_bytes_raw()
        ref = a.exchange(b.public_key()).hex()
        r1 = o.q(f"X25519 {sa.hex()} {pb.hex()}")
        r2 = o.q(f"X25519 {sb.hex()} {pa.hex()}")
        check(f"x25519[{i}].a", r1, ["1", ref])
        check(f"x25519[{i}].b", r2, ["1", ref])
    # Low-order agreement: u=0 and u=1 must be rejected (rc 0) by BOTH.
    # `cryptography` raises onCurve... verify: it refuses low-order?
    # (Record whatever it does — the C side must agree with rc semantics:
    # our oracle prints rc; compare against cryptography's accept/reject.)
    for u in (bytes(32), b"\x01" + bytes(31)):
        s = rng.randbytes(32)
        r = o.q(f"X25519 {s.hex()} {u.hex()}")
        try:
            ref = X25519PrivateKey.from_private_bytes(s).exchange(
                X25519PublicKey.from_public_bytes(u)).hex()
            want = ["1", ref]
        except Exception:
            want = ["0", "00" * 32]
        check(f"x25519-low[{u.hex()[:4]}]", r, want)

    # ---- SHA-256 / HMAC / HKDF ----
    import hashlib

    for i in range(n // 4):
        m = rng.randbytes(rng.choice([0, 1, 55, 56, 57, 64, 128, 300]))
        (got,) = o.q(f"SHA256 {hx(m)}")
        check(f"sha256[{i}]", got, hashlib.sha256(m).hexdigest())
        k = rng.randbytes(rng.choice([0, 16, 32, 64, 100, 131]))
        h = hmac_mod.HMAC(k, hashes.SHA256())
        h.update(m)
        (got,) = o.q(f"HMAC {hx(k)} {hx(m)}")
        check(f"hmac[{i}]", got, h.finalize().hex())
        salt = rng.randbytes(rng.choice([0, 13, 32]))
        ikm = rng.randbytes(rng.choice([11, 22, 64]))
        info = rng.randbytes(rng.choice([0, 10, 80]))
        olen = rng.choice([0, 32, 42, 82])
        prk, okm = o.q(f"HKDF {hx(salt)} {hx(ikm)} {hx(info)} {olen}")
        href = HKDF(algorithm=hashes.SHA256(), length=olen or 32,
                    salt=salt or None, info=info)
        # NOTE: cryptography requires length>=1; olen==0 tested C-side only.
        if olen == 0:
            exp_prk = hmac_mod.HMAC(
                salt or b"\x00" * 32, hashes.SHA256())
            exp_prk.update(ikm)
            check(f"hkdf-prk[{i}]", prk, exp_prk.finalize().hex())
            check(f"hkdf-okm[{i}]", okm, "-")
        else:
            ref = href.derive(ikm)
            # re-derive PRK independently for the extract comparison
            e = hmac_mod.HMAC(salt or b"\x00" * 32, hashes.SHA256())
            e.update(ikm)
            check(f"hkdf-prk[{i}]", prk, e.finalize().hex())
            check(f"hkdf-okm[{i}]", okm, ref.hex())

    # ---- ChaCha20-Poly1305 ----
    for i in range(n // 4):
        k = rng.randbytes(32)
        nn = rng.randbytes(12)
        aad = rng.randbytes(rng.choice([0, 8, 16, 100]))
        pt = rng.randbytes(rng.choice([0, 1, 15, 16, 64, 200]))
        ct, tag = o.q(f"AEAD_ENC {k.hex()} {nn.hex()} {hx(aad)} {hx(pt)}")
        a = ChaCha20Poly1305(k)
        ref = a.encrypt(nn, pt, aad or None)
        check(f"aead-ct[{i}]", [ct, tag],
              [ref[:-16].hex() or "-", ref[-16:].hex()])
        rc, back = o.q(
            f"AEAD_DEC {k.hex()} {nn.hex()} {hx(aad)} {ct} {tag}")
        check(f"aead-dec[{i}]", [rc, back], ["0", pt.hex() or "-"])
        # Tag tamper: flip a bit; both sides must reject.
        bad = bytearray.fromhex(tag)
        bad[0] ^= 1
        (rc,) = o.q(
            f"AEAD_DEC {k.hex()} {nn.hex()} {hx(aad)} {ct} {bad.hex()}")[:1]
        check(f"aead-tamper-oracle[{i}]", rc != "0", True)
        try:
            raw_ct = bytes.fromhex(ct) if ct != "-" else b""
            a.decrypt(nn, raw_ct + bad, aad or None)
            ref_reject = False
        except Exception:
            ref_reject = True
        check(f"aead-tamper-ref[{i}]", ref_reject, True)

    o.close()
    if fails:
        print(f"DIFFERENTIAL FAIL: {fails} mismatches")
        sys.exit(1)
    print(f"DIFFERENTIAL PASS: x25519 x{n}, sha/hmac/hkdf x{n // 4}, "
          f"aead x{n // 4} vs python-cryptography")
    sys.exit(0)


if __name__ == "__main__":
    main()
