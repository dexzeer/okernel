#!/usr/bin/env python3
"""A hostile TLS 1.2 server, written from RFC 5246 on its own primitives, against
our client (build-host/t_tlsa). Every case breaks one rule the client must
enforce, or does something unusual it must survive.

    python3 tests/tls12_evil.py [t_tlsa binary]        (run from the repo root)
"""
import datetime, hashlib, hmac, os, socket, struct, subprocess, sys, tempfile, threading
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

T = sys.argv[1] if len(sys.argv) > 1 else "build-host/t_tlsa"
PORT = 45144
HOST = "evil.example.com"
PROTO, MAC, CERT, DONE = 6, 3, 4, "ok"

def p16(v): return struct.pack(">H", v)
def p24(v): return struct.pack(">I", v)[1:]
def msg(t, body): return bytes([t]) + p24(len(body)) + body
def rec(t, body): return bytes([t, 3, 3]) + p16(len(body)) + body

def prf(secret, label, seed, n):
    seed = label + seed
    a, out = hmac.new(secret, seed, hashlib.sha256).digest(), b""
    while len(out) < n:
        out += hmac.new(secret, a + seed, hashlib.sha256).digest()
        a = hmac.new(secret, a, hashlib.sha256).digest()
    return out[:n]

# ---- certificates ----
def cert(subject, issuer, key, sign_key, ca, names=()):
    now = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
    b = (x509.CertificateBuilder()
         .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, subject)]))
         .issuer_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, issuer)]))
         .public_key(key.public_key()).serial_number(x509.random_serial_number())
         .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=30))
         .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
    if ca:
        b = b.add_extension(x509.KeyUsage(False, False, False, False, False, True, False, False, False), critical=True)
    else:
        b = (b.add_extension(x509.KeyUsage(True, False, False, False, False, False, False, False, False), critical=True)
             .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
             .add_extension(x509.SubjectAlternativeName([x509.DNSName(n) for n in names]), critical=False))
    return b.sign(sign_key, hashes.SHA256()).public_bytes(serialization.Encoding.DER)

root_key = rsa.generate_private_key(65537, 2048)
ROOT = cert("tls12 evil root", "tls12 evil root", root_key, root_key, True)
LEAF_KEY = rsa.generate_private_key(65537, 2048)
LEAF = cert(HOST, "tls12 evil root", LEAF_KEY, root_key, False, [HOST])
OTHER = cert("other.example.com", "tls12 evil root", LEAF_KEY, root_key, False, ["other.example.com"])
STRANGER_KEY = rsa.generate_private_key(65537, 2048)
P256_KEY = ec.generate_private_key(ec.SECP256R1())

# ---- one connection ----
class Conn:
    def __init__(self, s, mode):
        self.s, self.mode, self.buf = s, mode, b""
        self.sseq = self.cseq = 0

    def record(self):
        while len(self.buf) < 5 or len(self.buf) < 5 + struct.unpack(">H", self.buf[3:5])[0]:
            d = self.s.recv(65536)
            if not d: raise EOFError
            self.buf += d
        n = struct.unpack(">H", self.buf[3:5])[0]
        t, body, self.buf = self.buf[0], self.buf[5:5 + n], self.buf[5 + n:]
        return t, body

    def seal(self, t, pt):
        ex = struct.pack(">Q", self.sseq)
        aad = ex + bytes([t, 3, 3]) + p16(len(pt))
        ct = AESGCM(self.skey).encrypt(self.ssalt + ex, pt, aad)
        self.sseq += 1
        return rec(t, ex + ct)

    def open(self, t, body):
        aad = struct.pack(">Q", self.cseq) + bytes([t, 3, 3]) + p16(len(body) - 24)
        self.cseq += 1
        return AESGCM(self.ckey).decrypt(self.csalt + body[:8], body[8:], aad)

    def run(self):
        m = self.mode
        t, ch = self.record()
        assert t == 22 and ch[0] == 1
        crand = ch[6:38]
        srand = os.urandom(24) + (b"DOWNGRD\x01" if m == "downgrade" else b"\x11" * 8)
        exts = b""
        if m != "no_ri": exts += b"\xff\x01\x00\x01\x00"
        ems = m != "no_ems"
        if ems: exts += b"\x00\x17\x00\x00"
        suite = 0xC02B if m == "ecdsa_suite_rsa_cert" else 0xC013 if m == "cbc_suite" else 0xC02F
        sh = msg(2, b"\x03\x03" + srand + bytes([32]) + os.urandom(32) + p16(suite) + b"\x00" + p16(len(exts)) + exts)

        chain = [OTHER if m == "wrong_host" else LEAF, ROOT]
        cl = b"".join(p24(len(c)) + c for c in chain)
        certm = msg(11, p24(len(cl)) + cl)

        if m == "off_curve":
            group, pub, priv = 0x17, b"\x04" + b"\x01" * 64, None
        else:
            priv = X25519PrivateKey.generate()
            group = 0x1D
            pub = priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        params = b"\x03" + p16(group) + bytes([len(pub)]) + pub
        signed = crand + srand + params
        alg, key = 0x0401, LEAF_KEY
        if m == "ske_other_key": key = STRANGER_KEY
        if m == "ske_sha1":
            alg, sig = 0x0201, key.sign(signed, padding.PKCS1v15(), hashes.SHA1())
        elif m == "ske_pss":
            alg, sig = 0x0804, key.sign(signed, padding.PSS(padding.MGF1(hashes.SHA256()), 32), hashes.SHA256())
        else:
            sig = key.sign(signed, padding.PKCS1v15(), hashes.SHA256())
        if m == "ske_bad_sig": sig = sig[:20] + bytes([sig[20] ^ 1]) + sig[21:]
        ske = msg(12, params + p16(alg) + p16(len(sig)) + sig)
        done = msg(14, b"")

        flight = [sh, certm, ske, done]
        if m == "order": flight = [sh, ske, certm, done]
        if m == "skip_ske": flight = [sh, certm, done]
        if m == "two_certs": flight = [sh, certm, certm, ske, done]
        if m == "cert_request": flight = [sh, certm, ske, msg(13, b"\x01\x01\x00\x02\x04\x01\x00\x00"), done]
        if m == "trailing": flight = [sh, certm, ske, done, b"\x16"]
        if m == "empty_chain": flight = [sh, msg(11, p24(0)), ske, done]
        wire = b"".join(flight)
        if m == "one_record":
            self.s.sendall(rec(22, wire))
        elif m == "split_records":        # handshake messages cut across record boundaries, 700 bytes at a time
            for i in range(0, len(wire), 700): self.s.sendall(rec(22, wire[i:i + 700]))
        else:
            for f in flight: self.s.sendall(rec(22, f))

        transcript = wire if m != "trailing" else wire[:-1]
        transcript = ch + transcript
        t, b = self.record()
        if m == "cert_request":
            assert t == 22 and b == msg(11, p24(0)), "no empty Certificate"
            transcript += b
            t, b = self.record()
        assert t == 22 and b[0] == 16, "expected ClientKeyExchange"
        transcript += b
        shared = priv.exchange(X25519PublicKey.from_public_bytes(b[5:]))
        if ems: master = prf(shared, b"extended master secret", hashlib.sha256(transcript).digest(), 48)
        else: master = prf(shared, b"master secret", crand + srand, 48)
        kb = prf(master, b"key expansion", srand + crand, 40)
        self.ckey, self.skey, self.csalt, self.ssalt = kb[:16], kb[16:32], kb[32:36], kb[36:40]
        want_cf = prf(master, b"client finished", hashlib.sha256(transcript).digest(), 12)

        t, b = self.record()
        assert t == 20 and b == b"\x01", "expected ChangeCipherSpec"
        t, b = self.record()
        cf = self.open(22, b)
        assert t == 22 and cf == b"\x14\x00\x00\x0c" + want_cf, "the client's Finished is wrong"
        transcript += cf

        if m == "early_appdata": self.s.sendall(self.seal(23, b"too soon"))
        if m == "ticket_before_ccs": self.s.sendall(rec(22, msg(4, b"\x00" * 8)))
        if m == "plain_alert": self.s.sendall(rec(21, b"\x02\x28"))
        if m != "no_ccs": self.s.sendall(rec(20, b"\x01"))
        sf = prf(master, b"server finished", hashlib.sha256(transcript).digest(), 12)
        if m == "bad_finished": sf = bytes([sf[0] ^ 1]) + sf[1:]
        if m == "double_ccs": self.s.sendall(rec(20, b"\x01"))
        self.s.sendall(self.seal(22, b"\x14\x00\x00\x0c" + sf))

        t, b = self.record()
        req = self.open(23, b)
        assert t == 23 and req.startswith(b"GET /"), "expected the request"
        if m == "hello_request": self.s.sendall(self.seal(22, b"\x00\x00\x00\x00"))
        if m == "warning_alert": self.s.sendall(self.seal(21, b"\x01\x5a"))
        if m == "renegotiate": self.s.sendall(self.seal(22, msg(2, b"\x00" * 40)))
        if m == "ticket_after": self.s.sendall(self.seal(22, msg(4, b"\x00" * 8)))
        if m == "plain_close": self.s.sendall(rec(21, b"\x01\x00"))
        if m == "ccs_after": self.s.sendall(rec(20, b"\x01"))
        resp = self.seal(23, b"HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhello")
        if m == "tampered": resp = resp[:-1] + bytes([resp[-1] ^ 1])
        self.s.sendall(resp)
        self.s.sendall(self.seal(21, b"\x01\x00"))

CASES = [
    # the control group: all of these must work
    ("ok", DONE), ("no_ems", DONE), ("ske_pss", DONE), ("one_record", DONE), ("split_records", DONE),
    ("cert_request", DONE), ("hello_request", DONE), ("warning_alert", DONE),
    # version and hello
    ("downgrade", PROTO), ("no_ri", PROTO), ("cbc_suite", PROTO),
    # authentication
    ("ske_bad_sig", CERT), ("ske_other_key", CERT), ("ske_sha1", CERT), ("ecdsa_suite_rsa_cert", CERT), ("wrong_host", CERT),
    # flight shape
    ("order", PROTO), ("skip_ske", PROTO), ("two_certs", PROTO), ("trailing", PROTO), ("empty_chain", PROTO), ("off_curve", PROTO),
    # the end of the handshake
    ("bad_finished", MAC), ("no_ccs", PROTO), ("double_ccs", PROTO), ("early_appdata", PROTO), ("ticket_before_ccs", PROTO), ("plain_alert", PROTO),
    # application data
    ("tampered", MAC), ("renegotiate", PROTO), ("ticket_after", PROTO), ("plain_close", MAC), ("ccs_after", PROTO),
]

def main():
    d = tempfile.mkdtemp()
    root_path = os.path.join(d, "root.der")
    with open(root_path, "wb") as f: f.write(ROOT)
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", PORT))
    ls.listen(1)
    bad = 0
    for mode, want in CASES:
        err = []
        def serve():
            c, _ = ls.accept()
            c.settimeout(8)
            try: Conn(c, mode).run()
            except Exception as e: err.append(repr(e))     # a client that gives up looks like this too
            finally: c.close()
        th = threading.Thread(target=serve)
        th.start()
        env = dict(os.environ, TLSA_ROOT=root_path)
        r = subprocess.run([T, str(PORT)], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=40)
        th.join()
        out = r.stdout.decode(errors="replace")
        line = next((l for l in out.splitlines() if "run done" in l), "")
        reason = int(line.split("fail_reason=")[1]) if "fail_reason=" in line else -1
        success = "n=-1" not in line and "HTTP/1.0 200" in out
        good = success if want == DONE else (not success and reason == want)
        if not good: bad += 1
        print("%s %-22s want %-5s got %s%s" % ("PASS" if good else "FAIL", mode, want, "ok" if success else reason,
                                              "" if good or not err else "  server: " + err[0][:70]))
    print("tls12 evil: %d cases, %d wrong" % (len(CASES), bad))
    return 1 if bad else 0

sys.exit(main())
