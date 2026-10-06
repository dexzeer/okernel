#!/usr/bin/env python3
# evil_server.py — malicious TLS 1.3 server for red-teaming okernel's TLS client.
# Raw sockets, full message-level control (a TLS-Attacker equivalent without Java).
# Every attack holds a fully VALID handshake up to the mutation point, so any
# client completion (n>0) is a genuine bypass, never a framing artifact.
#
# Usage: evil_server.py --port P --attack NAME [--conns N]
#   Conns: sequential TCP connections to accept (needed for `twice` runs).
import argparse, hashlib, hmac as hmac_mod, os, socket, struct, sys, time
import datetime
from cryptography.hazmat.primitives.asymmetric import ec, padding
from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey, X25519PublicKey)
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography import x509 as cx509
from cryptography.x509 import ocsp as ocsp_mod
from cryptography.x509.oid import NameOID

FIX = "tests/adversarial"
HOST = "127.0.0.1"

# ---------------------------------------------------------------- utils

def u16(b, o=0): return (b[o] << 8) | b[o + 1]
def u24(b, o=0): return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]
def p16(v): return struct.pack(">H", v)
def p24(v): return bytes([(v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF])
def p32(v): return struct.pack(">I", v)

def hs_msg(t, body): return bytes([t]) + p24(len(body)) + body
def record(ct, payload, ver=0x0303):
    return bytes([ct]) + p16(ver) + p16(len(payload)) + payload

# Minimal DER walker: returns (tag, content, total_len) at offset.
def der_tlv(buf, off=0):
    tag = buf[off]
    lb = buf[off + 1]
    if lb & 0x80 == 0:
        ln, hb = lb, 2
    else:
        n = lb & 0x7F
        assert 1 <= n <= 4
        ln = int.from_bytes(buf[off + 2:off + 2 + n], "big")
        hb = 2 + n
    return tag, buf[off + hb:off + hb + ln], hb + ln

def der_enc(tag, content):
    ln = len(content)
    if ln < 128:
        return bytes([tag, ln]) + content
    lb = ln.to_bytes((ln.bit_length() + 7) // 8, "big")
    return bytes([tag, 0x80 | len(lb)]) + lb + content

# ---------------------------------------------------------------- key schedule (RFC 8446 §7, SHA-256)

def hkdf_extract(salt, ikm):
    return hmac_mod.new(salt, ikm, hashlib.sha256).digest()

def expand_label(secret, label, ctx, L):
    full = b"tls13 " + label
    info = p16(L) + bytes([len(full)]) + full + bytes([len(ctx)]) + ctx
    # HKDF-Expand
    out, t, c = b"", b"", 1
    while len(out) < L:
        t = hmac_mod.new(secret, t + info + bytes([c]), hashlib.sha256).digest()
        out += t
        c += 1
    return out[:L]

EMPTY_HASH = hashlib.sha256(b"").digest()
ZEROS = bytes(32)

class Sched:
    def __init__(self, shared, psk=None):
        early = hkdf_extract(ZEROS, psk if psk else ZEROS)
        self.early = early
        derived = expand_label(early, b"derived", EMPTY_HASH, 32)
        self.hs = hkdf_extract(derived, shared)
        d2 = expand_label(self.hs, b"derived", EMPTY_HASH, 32)
        self.master = hkdf_extract(d2, ZEROS)
        self.d2 = d2

# ---------------------------------------------------------------- AEAD

def aead_enc(key, iv, seq, inner_type, plaintext):
    nonce = bytearray(iv)
    for i in range(8):
        nonce[4 + i] ^= (seq >> (8 * (7 - i))) & 0xFF
    ct_body = bytes(plaintext) + bytes([inner_type])
    aead = ChaCha20Poly1305(key)
    # encrypt needs final length for aad: ct = body + 16 tag
    enc = aead.encrypt(bytes(nonce), ct_body,
                       b"\x17\x03\x03" + p16(len(ct_body) + 16))
    return record(23, enc)

def aead_dec(key, iv, seq, rec_payload):
    nonce = bytearray(iv)
    for i in range(8):
        nonce[4 + i] ^= (seq >> (8 * (7 - i))) & 0xFF
    aead = ChaCha20Poly1305(key)
    aad = b"\x17\x03\x03" + p16(len(rec_payload))
    pt = aead.decrypt(bytes(nonce), rec_payload, aad)
    # strip-first parse (mirrors client): trailing zeros, then type byte
    end = len(pt)
    while end > 0 and pt[end - 1] == 0:
        end -= 1
    if end == 0:
        raise ValueError("empty inner")
    return pt[:end - 1], pt[end - 1]

# ---------------------------------------------------------------- PKI

def load_der(p): return open(p, "rb").read()
def load_pem_key(p):
    return serialization.load_pem_private_key(open(p, "rb").read(), password=None)

class PKI:
    def __init__(self):
        self.int_der = load_der(f"{FIX}/at_int.der")
        self.int_key = load_pem_key(f"{FIX}/at_int.key")
        self.int_cert = cx509.load_der_x509_certificate(self.int_der)
        self.root_der = load_der(f"{FIX}/at_root.der")
        self.p384_int_der = load_der(f"{FIX}/at_p384_int.der")
        self.p384_int_key = load_pem_key(f"{FIX}/at_p384_int.key")
        self.p384_root_der = load_der(f"{FIX}/at_p384_root.der")
        # issuer subject element bytes (for OCSP byName splice checks)
        self.int_subject = extract_subject(self.int_der)
        # issuer SPKI key bytes (for OCSP key-hash self-check)
        self.int_keybytes = extract_spki_keybytes(self.int_der)

def extract_subject(cert_der):
    # Certificate SEQ -> TBS SEQ -> [0]ver? serial sigalg issuer(subject layout) ...
    _, c0, _ = der_tlv(cert_der, 0)
    _, tbs, _ = der_tlv(c0, 0)
    o = 0
    if tbs[o] == 0xA0:  # version
        _, _, tl = der_tlv(tbs, o); o += tl
    _, _, tl = der_tlv(tbs, o); o += tl  # serial
    _, _, tl = der_tlv(tbs, o); o += tl  # sig alg
    tag, _, tl = der_tlv(tbs, o)          # issuer Name
    assert tag == 0x30
    return tbs[o:o + tl]

def extract_spki_keybytes(cert_der):
    _, c0, _ = der_tlv(cert_der, 0)
    _, tbs, _ = der_tlv(c0, 0)
    o = 0
    if tbs[o] == 0xA0:
        _, _, tl = der_tlv(tbs, o); o += tl
    for _ in range(4):  # serial, sigalg, issuer, validity
        _, _, tl = der_tlv(tbs, o); o += tl
    _, _, tl = der_tlv(tbs, o); o += tl  # subject
    _, spki, _ = der_tlv(tbs, o)          # SPKI SEQ content
    so = 0
    _, _, tl = der_tlv(spki, so); so += tl  # alg
    tag, bits, _ = der_tlv(spki, so)        # BIT STRING
    assert tag == 0x03 and bits[0] == 0
    return bytes(bits[1:])

# ---------------------------------------------------------------- connection + CH parse

class Conn:
    def __init__(self, sock):
        self.s = sock
        self.s.settimeout(10)
        self.buf = b""
        self.c_hs_seq = 0
        self.s_hs_seq = 0
        self.c_ap_seq = 0
        self.s_ap_seq = 0

    def _fill(self, n):
        while len(self.buf) < n:
            d = self.s.recv(n - len(self.buf))
            if not d:
                raise ConnectionError("eof")
            self.buf += d

    def read_record(self):
        self._fill(5)
        ct, ver, ln = self.buf[0], u16(self.buf, 1), u16(self.buf, 3)
        self._fill(5 + ln)
        payload = self.buf[5:5 + ln]
        self.buf = self.buf[5 + ln:]
        return ct, ver, payload

    def send(self, data):
        self.s.sendall(data)

    def close(self):
        try: self.s.close()
        except OSError: pass

def parse_ch(body):
    o = 0
    assert u16(body, o) == 0x0303; o += 2
    rnd = body[o:o + 32]; o += 32
    sl = body[o]; o += 1
    sid = body[o:o + sl]; o += sl
    cl = u16(body, o); o += 2
    suites = [u16(body, o + i) for i in range(0, cl, 2)]; o += cl
    compl = body[o]; o += 1 + compl
    el = u16(body, o); o += 2
    exts = {}
    eend = o + el
    while o + 4 <= eend:
        t, ln = u16(body, o), u16(body, o + 2); o += 4
        exts[t] = body[o:o + ln]; o += ln
    # key share: first x25519 entry
    ke = None
    if 51 in exts:
        ks = exts[51]; p = 0
        ll = u16(ks, p); p += 2
        while p + 4 <= 2 + ll:
            g, kl = u16(ks, p), u16(ks, p + 2); p += 4
            if g == 0x001D and kl == 32 and ke is None:
                ke = ks[p:p + 32]
            p += kl
    # PSK offer
    psk = None
    if 41 in exts:
        pb = exts[41]; p = 0
        il = u16(pb, p); p += 2
        ticket = None; age = 0
        iend = p + il
        while p + 2 <= iend:
            tl = u16(pb, p); p += 2
            ticket = pb[p:p + tl]; p += tl
            age = int.from_bytes(pb[p:p + 4], "big"); p += 4
        bl = u16(pb, p); p += 2
        binders = []
        bend = p + bl
        while p < bend:
            ll2 = pb[p]; p += 1
            binders.append(pb[p:p + ll2]); p += ll2
        # trunc: full PSK body minus trailing (1+32) per binder
        trunc_drop = sum(1 + len(b) for b in binders)
        psk = {"ticket": ticket, "age": age, "binders": binders,
               "trunc_drop": trunc_drop, "binders_len": bl}
    # SNI
    sni = None
    if 0 in exts:
        sn = exts[0]; p = 0
        ll = u16(sn, p); p += 2
        if sn[p] == 0:
            nl = u16(sn, p + 1)
            sni = sn[p + 3:p + 3 + nl].decode("ascii", "replace")
    return {"random": rnd, "sid": sid, "suites": suites, "ke": ke,
            "psk": psk, "sni": sni, "exts": exts}

# ---------------------------------------------------------------- flight builders

def build_ee(alpn=True, extra_exts=(), dup_alpn=False, trailing=b""):
    exts = b""
    if alpn == "h2":
        body = p16(1 + 2) + bytes([2]) + b"h2"  # list_len=3? proto h2
        exts += p16(16) + p16(len(body)) + body
    elif alpn:
        body = p16(1 + 8) + bytes([8]) + b"http/1.1"
        exts += p16(16) + p16(len(body)) + body
        if dup_alpn:
            exts += p16(16) + p16(len(body)) + body
    for (t, b) in extra_exts:
        exts += p16(t) + p16(len(b)) + b
    exts += trailing
    return p16(len(exts)) + exts

def build_cert(entries):
    # entries: list of (der, exts_bytes)
    out = b""
    for der, exts in entries:
        out += p24(len(der)) + der + p16(len(exts)) + exts
    return bytes([0]) + p24(len(out)) + out

def staple_ext(ocsp_der):
    body = b"\x01" + p24(len(ocsp_der)) + ocsp_der
    return p16(5) + p16(len(body)) + body

CV_LABEL = b"TLS 1.3, server CertificateVerify"
def cv_content(th):
    return b"\x20" * 64 + CV_LABEL + b"\x00" + th
def cv_client_label_content(th):
    return b"\x20" * 64 + b"TLS 1.3, client CertificateVerify" + b"\x00" + th

def cv_msg(alg, sig):
    return hs_msg(15, p16(alg) + p16(len(sig)) + sig)

# ---------------------------------------------------------------- OCSP (real responses via cryptography)

def build_ocsp(pki, leaf_der, algo="sha1", status="good", stale=False,
               wrong_sig=False, responder=None, signer_key=None):
    leaf = cx509.load_der_x509_certificate(leaf_der)
    issuer = pki.int_cert
    now = datetime.datetime.utcnow().replace(microsecond=0)
    if stale:
        this_u = now - datetime.timedelta(days=30)
        next_u = now - datetime.timedelta(days=29)
    else:
        this_u = now - datetime.timedelta(minutes=5)
        next_u = now + datetime.timedelta(days=1)
    h = hashes.SHA1() if algo == "sha1" else hashes.SHA256()
    st = {"good": ocsp_mod.OCSPCertStatus.GOOD,
          "revoked": ocsp_mod.OCSPCertStatus.REVOKED,
          "unknown": ocsp_mod.OCSPCertStatus.UNKNOWN}[status]
    b = ocsp_mod.OCSPResponseBuilder()
    b = b.add_response(cert=leaf, issuer=issuer, algorithm=h,
                       cert_status=st, this_update=this_u,
                       next_update=next_u,
                       revocation_time=this_u if status == "revoked" else None,
                       revocation_reason=None)
    resp_cert = responder or issuer
    b = b.responder_id(ocsp_mod.OCSPResponderEncoding.NAME, resp_cert)
    key = signer_key or pki.int_key
    sigalg = hashes.SHA256()
    resp = b.sign(key, sigalg)
    der = resp.public_bytes(serialization.Encoding.DER)
    if wrong_sig:
        # flip a byte deep inside signatureValue (near end of response)
        der = bytearray(der)
        der[-5] ^= 0xFF
        der = bytes(der)
    return der

def ocsp_selfcheck(resp_der, leaf_der, int_der, algo="sha1"):
    # parse certID out of the response and compare with recomputed values
    try:
        _, r0, _ = der_tlv(resp_der, 0)
        o = 0
        _, _, tl = der_tlv(r0, o); o += tl      # status ENUM
        _, rb, _ = der_tlv(r0, o)                # [0] responseBytes
        _, rbs, _ = der_tlv(rb, 0)
        q = 0
        _, _, tl = der_tlv(rbs, q); q += tl      # OID
        _, roct, _ = der_tlv(rbs, q)             # OCTET STRING
        _, basic, _ = der_tlv(roct, 0)
        t = 0
        _, tbs, _ = der_tlv(basic, t)            # tbsResponseData
        # responderID [1]: extract byName content
        r = 0
        if tbs[r] == 0xA0:
            _, _, tl = der_tlv(tbs, r); r += tl
        assert tbs[r] == 0xA1, "responder not byName"
        _, byname, _ = der_tlv(tbs, r)
        # responses -> single -> certID
        # (walk: producedAt, responses SEQ)
        while r < len(tbs) and tbs[r] not in (0x17, 0x18, 0x30):
            _, _, tl = der_tlv(tbs, r); r += tl
        # find responses SEQ: next 0x30 after producedAt time
        if tbs[r] in (0x17, 0x18):
            _, _, tl = der_tlv(tbs, r); r += tl
        _, resps, _ = der_tlv(tbs, r)
        s = 0
        _, single, _ = der_tlv(resps, s)
        u = 0
        _, certid, _ = der_tlv(single, u)
        v = 0
        _, halg, _ = der_tlv(certid, v)
        w = 0
        _, ho, _ = der_tlv(halg, w)
        _, inh, _ = der_tlv(certid, v + len(der_enc(0x30, halg)))
        # recompute
        int_subject = extract_subject(int_der)
        int_kb = extract_spki_keybytes(int_der)
        hh = hashlib.sha1 if algo == "sha1" else hashlib.sha256
        want_nh = hh(int_subject).digest()
        want_kh = hh(int_kb).digest()
        # inh/ikh are OCTET STRINGs: re-walk properly
        vv = 0
        _, _, tl = der_tlv(certid, vv); vv += tl  # hash alg seq
        _, i1, t1 = der_tlv(certid, vv); ih = i1; vv += t1
        _, i2, t2 = der_tlv(certid, vv); kh = i2; vv += t2
        _, sder, _ = der_tlv(certid, vv)
        leaf = cx509.load_der_x509_certificate(leaf_der)
        serial_ok = (int.from_bytes(sder, "big") == leaf.serial_number)
        return (bytes(ih) == want_nh and bytes(kh) == want_kh and
                serial_ok and bytes(byname) == int_subject)
    except Exception as e:
        return f"selfcheck-error: {e}"

# ---------------------------------------------------------------- server flows

class Srv:
    def __init__(self, pki):
        self.pki = pki
        self.priv = X25519PrivateKey.generate()
        self.pub = self.priv.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self.tickets = {}  # ticket -> (res_master, nonce)

    def common_sh(self, ch, group=0x001D, ke=None, extra_exts=None,
                  omit_sv=False, omit_ks=False, dup_sv=False, ver=0x0303,
                  comp=0, suite=0x1303, sid=None, ks_trailer=b"",
                  many_exts=0, random=None):
        rnd = random or os.urandom(32)
        sid = ch["sid"] if sid is None else sid
        body = p16(ver) + rnd
        body += bytes([len(sid)]) + sid
        body += p16(suite) + bytes([comp])
        exts = b""
        if not omit_sv:
            sv = p16(0x0304)
            exts += p16(43) + p16(len(sv)) + sv
            if dup_sv:
                exts += p16(43) + p16(len(sv)) + sv
        if not omit_ks:
            k = ke or self.pub
            kse = p16(group) + p16(len(k)) + k + ks_trailer
            exts += p16(51) + p16(len(kse)) + kse
        if extra_exts:
            exts += extra_exts
        for i in range(many_exts):
            exts += p16(0x1000 + i) + p16(0)
        body += p16(len(exts)) + exts
        return body, rnd

    def derive_hs(self, ch_ke, psk=None):
        peer = X25519PublicKey.from_public_bytes(ch_ke)
        shared = self.priv.exchange(peer)
        return shared, Sched(shared, psk)

    def send_stream(self, conn, key, iv, seq, data, frag=None):
        n = len(data) if frag is None else frag
        for i in range(0, len(data), n):
            conn.send(aead_enc(key, iv, seq[0], 22, data[i:i + n]))
            seq[0] += 1

def read_ch(conn):
    ct, ver, payload = conn.read_record()
    assert ct == 22, f"first record type {ct}"
    assert payload[0] == 1, "not a ClientHello"
    bl = u24(payload, 1)
    body = payload[4:]
    while len(body) < bl:  # (client never fragments; be lenient)
        ct2, _, p2 = conn.read_record()
        assert ct2 == 22
        body += p2
    body = body[:bl]
    ch = parse_ch(body)
    ch["msg"] = bytes([1]) + p24(bl) + body
    ch["body"] = body
    assert ch["ke"], "no x25519 share"
    return ch

def thash(*msgs):
    h = hashlib.sha256()
    for m in msgs:
        h.update(m)
    return h.digest()

def flight_keys(sched, th):
    c_hs = expand_label(sched.hs, b"c hs traffic", th, 32)
    s_hs = expand_label(sched.hs, b"s hs traffic", th, 32)
    ck, ci = expand_label(c_hs, b"key", b"", 32), expand_label(c_hs, b"iv", b"", 12)
    sk, si = expand_label(s_hs, b"key", b"", 32), expand_label(s_hs, b"iv", b"", 12)
    s_fin = expand_label(s_hs, b"finished", b"", 32)
    return (c_hs, s_hs, ck, ci, sk, si, s_fin)

def sign_cv_ecdsa(key, data):
    return key.sign(data, ec.ECDSA(hashes.SHA256()))

def read_client_finished_unused():
    # (removed helper — drain_client_finished below is the real path)
    raise AssertionError("unused")

def drain_client_finished(conn, ck, ci, cseq, c_hs_secret, th_through_sfin):
    c_fin = expand_label(c_hs_secret, b"finished", b"", 32)
    buf = b""
    while True:
        ct, ver, payload = conn.read_record()
        if ct == 20:  # CCS
            continue
        assert ct == 23, f"expected encrypted, got {ct}"
        pt, itype = aead_dec(ck, ci, cseq[0], payload)
        cseq[0] += 1
        assert itype == 22, f"inner type {itype}, expected handshake"
        buf += pt
        if len(buf) >= 4:
            t, bl = buf[0], u24(buf, 1)
            if len(buf) >= 4 + bl:
                assert t == 20 and bl == 32, f"client finished shape {t}/{bl}"
                mac = buf[4:4 + 32]
                want = hmac_mod.new(c_fin, th_through_sfin, hashlib.sha256).digest()
                assert mac == want, "client Finished MAC mismatch (server bug?)"
                return buf[4 + bl:]

def read_app_request(conn, ck, ci, cseq):
    data = b""
    while b"\r\n\r\n" not in data:
        ct, ver, payload = conn.read_record()
        assert ct == 23
        pt, itype = aead_dec(ck, ci, cseq[0], payload)
        cseq[0] += 1
        assert itype == 23, f"inner {itype}"
        data += pt
    return data

HTTP_RESP = (b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
             b"Connection: close\r\n\r\nhello")

def send_nst(conn, sk, si, sseq, srv, res_master, lifetime=100):
    nonce = os.urandom(8)
    ticket = os.urandom(32)
    age_add = int.from_bytes(os.urandom(4), "big")
    body = (p32(lifetime) + p32(age_add) + bytes([len(nonce)]) + nonce +
            p16(len(ticket)) + ticket + p16(0))
    srv.tickets[ticket] = (res_master, nonce, age_add, lifetime)
    conn.send(aead_enc(sk, si, sseq[0], 22, hs_msg(4, body)))
    sseq[0] += 1

def send_app(conn, sk, si, sseq, data, pad=0):
    conn.send(aead_enc(sk, si, sseq[0], 23, bytes(data) + b"\x00" * pad))
    sseq[0] += 1

def app_keys(sched, th_sfin):
    c_ap = expand_label(sched.master, b"c ap traffic", th_sfin, 32)
    s_ap = expand_label(sched.master, b"s ap traffic", th_sfin, 32)
    ck = expand_label(c_ap, b"key", b"", 32); ci = expand_label(c_ap, b"iv", b"", 12)
    sk = expand_label(s_ap, b"key", b"", 32); si = expand_label(s_ap, b"iv", b"", 12)
    return ck, ci, sk, si

def res_master_for(sched, th_both):
    return expand_label(sched.master, b"res master", th_both, 32)

# ---------------------------------------------------------------- attacks

ATK = {}
def attack(name):
    def deco(fn):
        ATK[name] = fn
        return fn
    return deco

def leaf_chain(pki, leaf):
    return [(leaf, b""), (pki.int_der, b""), (pki.root_der, b"")]

@attack("base-ec")
def a_base_ec(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec")

@attack("base-rsa")
def a_base_rsa(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_rsa_leaf.der")),
                   load_pem_key(f"{FIX}/at_rsa_leaf.key"), "rsa-pss")

@attack("base-p384")
def a_base_p384(srv, conn, idx):
    pki = srv.pki
    return do_full(srv, conn,
                   [(load_der(f"{FIX}/at_p384_leaf.der"), b""),
                    (pki.p384_int_der, b""), (pki.p384_root_der, b"")],
                   load_pem_key(f"{FIX}/at_p384_leaf.key"), "ec384")

def do_full(srv, conn, entries, leaf_key, cvkind, ee=None, frag=None,
            coalesce=False, staple_map=None, cv_mut=None, cv_alg_override=None):
    pki = srv.pki
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    sseq, cseq = [0], [0]
    ee_body = build_ee() if ee is None else ee
    ee_msg = hs_msg(8, ee_body)
    # staple injection
    if staple_map:
        new_entries = []
        for i, (der, exts) in enumerate(entries):
            if i in staple_map:
                exts = staple_ext(staple_map[i]) + exts
            new_entries.append((der, exts))
        entries = new_entries
    cert_body = build_cert(entries)
    cert_msg = hs_msg(11, cert_body)
    th_cert = thash(ch["msg"], sh_msg, ee_msg, cert_msg)
    data = cv_content(th_cert)
    if cvkind == "ec":
        sig = sign_cv_ecdsa(leaf_key, data); alg = 0x0403
    elif cvkind == "ec384":
        sig = leaf_key.sign(data, ec.ECDSA(hashes.SHA384())); alg = 0x0503
    elif cvkind == "rsa-pss":
        sig = leaf_key.sign(data, padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                             salt_length=32), hashes.SHA256())
        alg = 0x0804
    else:
        raise AssertionError("cvkind")
    if cv_mut:
        sig, alg = cv_mut(sig, alg, data, leaf_key)
    if cv_alg_override is not None:
        alg = cv_alg_override
    cv_m = cv_msg(alg, sig)
    th_pre = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m)
    fin = hmac_mod.new(s_fin, th_pre, hashlib.sha256).digest()
    fin_m = hs_msg(20, fin)
    flight = ee_msg + cert_msg + cv_m + fin_m
    if coalesce:
        srv.send_stream(conn, sk, si, sseq, flight, frag=None)
    else:
        srv.send_stream(conn, sk, si, sseq, flight, frag=frag)
    # app keys
    th_sfin = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m, fin_m)
    ck2, ci2, sk2, si2 = app_keys(sched, th_sfin)
    cseq2, sseq2 = [0], [0]
    # client finished (hs keys)
    c_hs_fin = expand_label(c_hs, b"finished", b"", 32)
    drain_client_finished(conn, ck, ci, cseq, c_hs, th_sfin)
    req = read_app_request(conn, ck2, ci2, cseq2)
    assert b"GET" in req, f"no GET: {req[:40]}"
    th_both = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m, fin_m,
                    hs_msg(20, hmac_mod.new(c_hs_fin, th_sfin, hashlib.sha256).digest()))
    rm = res_master_for(sched, th_both)
    send_nst(conn, sk2, si2, sseq2, srv, rm)
    send_nst(conn, sk2, si2, sseq2, srv, rm)
    send_app(conn, sk2, si2, sseq2, HTTP_RESP)
    try:
        conn.s.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    return "served"

# --- CV forgery helpers
def mut_garbage(sig, alg, data, key):
    return os.urandom(len(sig)), alg

def mut_client_label(sig, alg, data, key):
    th = data[-32:]
    return sign_cv_ecdsa(key, cv_client_label_content(th)), alg

def mut_wrongtx(sig, alg, data, key):
    bad = bytearray(data); bad[-1] ^= 0x01
    return sign_cv_ecdsa(key, bytes(bad)), alg

def split_sig(sig):
    # returns (r_value, s_value); asserts exact DER shape (no trailing junk)
    tag, seq, _ = der_tlv(sig, 0)
    assert tag == 0x30, "sig not a SEQUENCE"
    t2, rval, rtl = der_tlv(seq, 0)
    assert t2 == 0x02, "r not INTEGER"
    t3, sval, stl = der_tlv(seq, rtl)
    assert t3 == 0x02, "s not INTEGER"
    assert rtl + stl == len(seq), "trailing bytes in sig SEQ"
    return bytes(rval), bytes(sval)

def mut_padr(sig, alg, data, key):
    # re-encode DER with a non-minimal leading zero on r (double pad)
    rval, sval = split_sig(sig)
    new_r = b"\x00" + rval  # minimal rval never starts 0x00 -> always non-minimal now
    assert len(new_r) == len(rval) + 1
    return der_enc(0x30, der_enc(0x02, new_r) + der_enc(0x02, sval)), alg

def mut_r_plus_n(sig, alg, data, key):
    # r' = r + n (P-256 order) — must be rejected, not reduced
    N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
    _, r, _ = der_tlv(sig, 0)
    _, rcont, rtl = der_tlv(r, 0)
    rest = r[rtl:]
    rv = int.from_bytes(rcont, "big") + N
    rb = rv.to_bytes((rv.bit_length() + 7) // 8, "big")
    if rb[0] & 0x80:
        rb = b"\x00" + rb
    return der_enc(0x30, der_enc(0x02, rb) + rest), alg

def mut_s_zero(sig, alg, data, key):
    # s = 0 (zero scalar forbidden)
    rval, _ = split_sig(sig)
    return der_enc(0x30, der_enc(0x02, rval) + der_enc(0x02, b"\x00")), alg

@attack("cv-garbage")
def a_cv_garbage(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", cv_mut=mut_garbage)

@attack("cv-wrongtx")
def a_cv_wrongtx(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", cv_mut=mut_wrongtx)

@attack("cv-clientlabel")
def a_cv_clientlabel(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", cv_mut=mut_client_label)

@attack("cv-padr")
def a_cv_padr(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", cv_mut=mut_padr)

@attack("cv-rgen")
def a_cv_rgen(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", cv_mut=mut_r_plus_n)

@attack("cv-szero")
def a_cv_szero(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", cv_mut=mut_s_zero)

@attack("cv-wrongalg")
def a_cv_wrongalg(srv, conn, idx):
    # ECDSA sig bytes advertised as RSA-PSS: parse gate passes, key-type gate fails
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", cv_alg_override=0x0804)

@attack("cv-pkcs1")
def a_cv_pkcs1(srv, conn, idx):
    # real PKCS#1 v1.5 sig over correct data, alg 0x0401: RFC forbids in CV
    def mut(sig, alg, data, key):
        s = key.sign(data, padding.PKCS1v15(), hashes.SHA256())
        return s, 0x0401
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_rsa_leaf.der")),
                   load_pem_key(f"{FIX}/at_rsa_leaf.key"), "rsa-pss", cv_mut=mut)

@attack("cv-pss-salt20")
def a_cv_pss_salt20(srv, conn, idx):
    def mut(sig, alg, data, key):
        s = key.sign(data, padding.PSS(mgf=padding.MGF1(hashes.SHA256()),
                                      salt_length=20), hashes.SHA256())
        return s, 0x0804
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_rsa_leaf.der")),
                   load_pem_key(f"{FIX}/at_rsa_leaf.key"), "rsa-pss", cv_mut=mut)

@attack("cv-pss-mgf384")
def a_cv_pss_mgf384(srv, conn, idx):
    def mut(sig, alg, data, key):
        s = key.sign(data, padding.PSS(mgf=padding.MGF1(hashes.SHA384()),
                                      salt_length=32), hashes.SHA256())
        return s, 0x0804
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_rsa_leaf.der")),
                   load_pem_key(f"{FIX}/at_rsa_leaf.key"), "rsa-pss", cv_mut=mut)

# --- chain attacks
def chain_attack(leaf_name, extra=(), cvkind="ec", keypath=None):
    def fn(srv, conn, idx):
        leaf = load_der(f"{FIX}/{leaf_name}.der")
        key = load_pem_key(keypath or f"{FIX}/{leaf_name}.key")
        entries = [(leaf, b"")] + [(d, b"") for d in extra]
        return do_full(srv, conn, entries, key, cvkind)
    return fn

ATK["ch-wronghost"] = chain_attack("at_dnsip_leaf")
ATK["ch-expired"] = chain_attack("at_leaf_expired")
ATK["ch-ku"] = chain_attack("at_ku_leaf")
ATK["ch-eku"] = chain_attack("at_eku_leaf")
ATK["ch-crit"] = chain_attack("at_crit_leaf")
ATK["ch-rsa1024"] = chain_attack("at_evil_rsa1024", cvkind="rsa-pss",
                                 keypath="tests/adversarial/at_evil_rsa1024.key")
ATK["ch-rogue"] = lambda srv, conn, idx: do_full(
    srv, conn, [(load_der(f"{FIX}/at_evil_rogue.der"), b"")],
    load_pem_key("tests/adversarial/at_evil_rogue.key"), "ec")
ATK["ch-v1"] = chain_attack("at_evil_v1", keypath="tests/adversarial/at_evil_v1.key")
ATK["ch-emptycert"] = lambda srv, conn, idx: do_empty_cert(srv, conn, idx)

def do_empty_cert(srv, conn, idx):
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    sseq = [0]
    ee_msg = hs_msg(8, build_ee())
    cert_msg = hs_msg(11, bytes([0]) + p24(0))  # empty list
    srv.send_stream(conn, sk, si, sseq, ee_msg + cert_msg)
    time.sleep(0.3)
    return "empty-sent"

@attack("ch-reordered")
def a_ch_reordered(srv, conn, idx):
    pki = srv.pki
    leaf = load_der(f"{FIX}/at_leaf.der")
    key = load_pem_key(f"{FIX}/at_leaf.key")
    # int FIRST (violates leaf-first order)
    return do_full(srv, conn, [(pki.int_der, b""), (leaf, b""),
                               (pki.root_der, b"")], key, "ec")

@attack("ch-noca")
def a_ch_noca(srv, conn, idx):
    pki = srv.pki
    leaf = load_der(f"{FIX}/at_evil_nocaleaf.der")
    key = load_pem_key("tests/adversarial/at_evil_nocaleaf.key")
    noca = load_der(f"{FIX}/at_evil_nocaint.der")
    return do_full(srv, conn, [(leaf, b""), (noca, b""),
                               (pki.int_der, b""), (pki.root_der, b"")],
                   key, "ec")

@attack("ch-kubadpad")
def a_ch_kubadpad(srv, conn, idx):
    # same-length KU splice: padding bit set with unused=7
    der = bytearray(load_der(f"{FIX}/at_leaf.der"))
    ku_oid = bytes([0x06, 0x03, 0x55, 0x1D, 0x0F])
    i = bytes(der).find(ku_oid)
    assert i > 0, "KU ext not found"
    # ext SEQ: OID, [critical], OCTET{ BITSTRING{unused, bits} }
    # find BIT STRING 03 02 after i
    j = bytes(der).find(b"\x03\x02", i)
    assert j > 0
    assert der[j + 2] <= 7  # unused-bits
    der[j + 3] |= 0x01      # set a padding bit -> invalid
    return do_full(srv, conn, leaf_chain(srv.pki, bytes(der)),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec")

# --- state machine / record attacks
def sh_only_attack(mod):
    def fn(srv, conn, idx):
        ch = read_ch(conn)
        srv.derive_hs(ch["ke"])
        sh_body, _ = srv.common_sh(ch, **mod)
        conn.send(record(22, hs_msg(2, sh_body)))
        time.sleep(0.3)
        return "sh-sent"
    return fn

ATK["sm-hrr"] = sh_only_attack(
    {"random": bytes.fromhex("CF21AD74E59A6111BE1D8C021E65B891") + bytes(16)})
ATK["sm-sid0"] = sh_only_attack({"sid": b""})
ATK["sm-suite1301"] = sh_only_attack({"suite": 0x1301})
ATK["sm-group17"] = sh_only_attack(
    {"group": 0x0017, "ke": bytes([4]) + os.urandom(64)})
ATK["sm-nosv"] = sh_only_attack({"omit_sv": True})
ATK["sm-noks"] = sh_only_attack({"omit_ks": True})
ATK["sm-dupsh"] = sh_only_attack({"dup_sv": True})
ATK["sm-kstrail"] = sh_only_attack({"ks_trailer": b"\xde\xad\xbe\xef"})
ATK["sm-ver0304"] = sh_only_attack({"ver": 0x0304})
ATK["sm-comp1"] = sh_only_attack({"comp": 1})
ATK["sm-17exts"] = sh_only_attack({"many_exts": 17})

@attack("sm-shtrail")
def a_sm_shtrail(srv, conn, idx):
    ch = read_ch(conn)
    srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    conn.send(record(22, hs_msg
(2, sh_body) + b"\xaa\xbb\xcc"))
    time.sleep(0.3)
    return "sh-trail-sent"

@attack("sm-cvtrail")
def a_sm_cvtrail(srv, conn, idx):
    return do_full_cvtrail(srv, conn)

def do_full_cvtrail(srv, conn):
    pki = srv.pki
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    sseq = [0]
    ee_msg = hs_msg(8, build_ee())
    cert_body = build_cert(leaf_chain(pki, load_der(f"{FIX}/at_leaf.der")))
    cert_msg = hs_msg(11, cert_body)
    leaf_key = load_pem_key(f"{FIX}/at_leaf.key")
    th_cert = thash(ch["msg"], sh_msg, ee_msg, cert_msg)
    sig = sign_cv_ecdsa(leaf_key, cv_content(th_cert))
    # CV body with 2 trailing garbage bytes (exact-framing violation)
    cv_m = hs_msg(15, p16(0x0403) + p16(len(sig)) + sig) + b"\xde\xad"
    srv.send_stream(conn, sk, si, sseq, ee_msg + cert_msg + cv_m)
    time.sleep(0.3)
    return "cvtrail-sent"

@attack("sm-fin31")
def a_sm_fin31(srv, conn, idx):
    return do_fin_mut(srv, conn, 31)

@attack("sm-fintrail")
def a_sm_fintrail(srv, conn, idx):
    return do_fin_mut(srv, conn, 32, trail=True)

def do_fin_mut(srv, conn, fin_len, trail=False):
    pki = srv.pki
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    sseq = [0]
    ee_msg = hs_msg(8, build_ee())
    cert_body = build_cert(leaf_chain(pki, load_der(f"{FIX}/at_leaf.der")))
    cert_msg = hs_msg(11, cert_body)
    leaf_key = load_pem_key(f"{FIX}/at_leaf.key")
    th_cert = thash(ch["msg"], sh_msg, ee_msg, cert_msg)
    sig = sign_cv_ecdsa(leaf_key, cv_content(th_cert))
    cv_m = cv_msg(0x0403, sig)
    th_pre = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m)
    fin = hmac_mod.new(s_fin, th_pre, hashlib.sha256).digest()[:fin_len]
    fin_m = hs_msg(20, fin)
    if trail:
        fin_m += b"\x00"  # trailing byte after Finished in reassembly
    srv.send_stream(conn, sk, si, sseq, ee_msg + cert_msg + cv_m + fin_m)
    time.sleep(0.3)
    return "finmut-sent"

@attack("sm-loworder")
def a_sm_loworder(srv, conn, idx):
    ch = read_ch(conn)
    srv.priv.exchange(X25519PublicKey.from_public_bytes(ch["ke"]))
    sh_body, _ = srv.common_sh(ch, ke=bytes(32))
    conn.send(record(22, hs_msg(2, sh_body)))
    time.sleep(0.3)
    return "loworder-sent"

@attack("sm-eeh2")
def a_sm_eeh2(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", ee=build_ee(alpn="h2"))

@attack("sm-eedup")
def a_sm_eedup(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", ee=build_ee(dup_alpn=True))

@attack("sm-eetrail")
def a_sm_eetrail(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", ee=build_ee(trailing=b"\x00"))

@attack("sm-staplebig")
def a_sm_staplebig(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec",
                   staple_map={0: b"X" * 3000})

@attack("sm-keyupdate")
def a_sm_keyupdate(srv, conn, idx):
    pki = srv.pki
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    sseq, cseq = [0], [0]
    ee_msg = hs_msg(8, build_ee())
    cert_body = build_cert(leaf_chain(pki, load_der(f"{FIX}/at_leaf.der")))
    cert_msg = hs_msg(11, cert_body)
    leaf_key = load_pem_key(f"{FIX}/at_leaf.key")
    th_cert = thash(ch["msg"], sh_msg, ee_msg, cert_msg)
    cv_m = cv_msg(0x0403, sign_cv_ecdsa(leaf_key, cv_content(th_cert)))
    th_pre = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m)
    fin_m = hs_msg(20, hmac_mod.new(s_fin, th_pre, hashlib.sha256).digest())
    srv.send_stream(conn, sk, si, sseq, ee_msg + cert_msg + cv_m + fin_m)
    th_sfin = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m, fin_m)
    ck2, ci2, sk2, si2 = app_keys(sched, th_sfin)
    cseq2, sseq2 = [0], [0]
    c_hs_fin = expand_label(c_hs, b"finished", b"", 32)
    drain_client_finished(conn, ck, ci, cseq, c_hs, th_sfin)
    read_app_request(conn, ck2, ci2, cseq2)
    # forbidden post-handshake message instead of application data
    conn.send(aead_enc(sk2, si2, sseq2[0], 22, hs_msg(24, b"\x00")))
    sseq2[0] += 1
    time.sleep(0.3)
    return "keyupdate-sent"

@attack("sm-earlynst")
def a_sm_earlynst(srv, conn, idx):
    # NST immediately after server Finished (client still in RECV_HS)
    pki = srv.pki
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    sseq = [0]
    ee_msg = hs_msg(8, build_ee())
    cert_body = build_cert(leaf_chain(pki, load_der(f"{FIX}/at_leaf.der")))
    cert_msg = hs_msg(11, cert_body)
    leaf_key = load_pem_key(f"{FIX}/at_leaf.key")
    th_cert = thash(ch["msg"], sh_msg, ee_msg, cert_msg)
    cv_m = cv_msg(0x0403, sign_cv_ecdsa(leaf_key, cv_content(th_cert)))
    th_pre = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m)
    fin_m = hs_msg(20, hmac_mod.new(s_fin, th_pre, hashlib.sha256).digest())
    nst = hs_msg(4, p32(100) + p32(7) + bytes([8]) + os.urandom(8) +
                 p16(32) + os.urandom(32) + p16(0))
    srv.send_stream(conn, sk, si, sseq, ee_msg + cert_msg + cv_m + fin_m + nst)
    time.sleep(0.3)
    return "earlynst-sent"

@attack("staple-badsig")
def a_staple_badsig(srv, conn, idx):
    pki = srv.pki
    ocsp_der = build_ocsp(pki, load_der(f"{FIX}/at_leaf.der"), wrong_sig=True)
    return do_full(srv, conn, leaf_chain(pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec",
                   staple_map={0: ocsp_der})

@attack("staple-wrongserial")
def a_staple_wrongserial(srv, conn, idx):
    pki = srv.pki
    # staple answers for a DIFFERENT cert (dnsip leaf) than the served leaf
    ocsp_der = build_ocsp(pki, load_der(f"{FIX}/at_dnsip_leaf.der"))
    return do_full(srv, conn, leaf_chain(pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec",
                   staple_map={0: ocsp_der})

@attack("staple-stale")
def a_staple_stale(srv, conn, idx):
    pki = srv.pki
    ocsp_der = build_ocsp(pki, load_der(f"{FIX}/at_leaf.der"), stale=True)
    return do_full(srv, conn, leaf_chain(pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec",
                   staple_map={0: ocsp_der})

@attack("staple-revoked")
def a_staple_revoked(srv, conn, idx):
    pki = srv.pki
    ocsp_der = build_ocsp(pki, load_der(f"{FIX}/at_leaf.der"), status="revoked")
    return do_full(srv, conn, leaf_chain(pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec",
                   staple_map={0: ocsp_der})

@attack("staple-responder")
def a_staple_responder(srv, conn, idx):
    pki = srv.pki
    # responder is a DIFFERENT CA (p384 int), signed by its key
    p384_cert = cx509.load_der_x509_certificate(pki.p384_int_der)
    ocsp_der = build_ocsp(pki, load_der(f"{FIX}/at_leaf.der"),
                          responder=p384_cert, signer_key=pki.p384_int_key)
    return do_full(srv, conn, leaf_chain(pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec",
                   staple_map={0: ocsp_der})

@attack("muststaple-none")
def a_muststaple_none(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_ms_leaf.der")),
                   load_pem_key(f"{FIX}/at_ms_leaf.key"), "ec")

@attack("sm-plainclose")
def a_sm_plainclose(srv, conn, idx):
    pki = srv.pki
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    sseq, cseq = [0], [0]
    ee_msg = hs_msg(8, build_ee())
    cert_body = build_cert(leaf_chain(pki, load_der(f"{FIX}/at_leaf.der")))
    cert_msg = hs_msg(11, cert_body)
    leaf_key = load_pem_key(f"{FIX}/at_leaf.key")
    th_cert = thash(ch["msg"], sh_msg, ee_msg, cert_msg)
    cv_m = cv_msg(0x0403, sign_cv_ecdsa(leaf_key, cv_content(th_cert)))
    th_pre = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m)
    fin_m = hs_msg(20, hmac_mod.new(s_fin, th_pre, hashlib.sha256).digest())
    srv.send_stream(conn, sk, si, sseq, ee_msg + cert_msg + cv_m + fin_m)
    th_sfin = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m, fin_m)
    ck2, ci2, sk2, si2 = app_keys(sched, th_sfin)
    cseq2 = [0]
    c_hs_fin = expand_label(c_hs, b"finished", b"", 32)
    drain_client_finished(conn, ck, ci, cseq, c_hs, th_sfin)
    read_app_request(conn, ck2, ci2, cseq2)
    # plaintext close_notify AFTER handshake: unauthenticated, must be PROTO
    conn.send(record(21, b"\x01\x00"))
    time.sleep(0.3)
    return "plainclose-sent"

@attack("sm-encclose")
def a_sm_encclose(srv, conn, idx):
    pki = srv.pki
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    sseq, cseq = [0], [0]
    ee_msg = hs_msg(8, build_ee())
    cert_body = build_cert(leaf_chain(pki, load_der(f"{FIX}/at_leaf.der")))
    cert_msg = hs_msg(11, cert_body)
    leaf_key = load_pem_key(f"{FIX}/at_leaf.key")
    th_cert = thash(ch["msg"], sh_msg, ee_msg, cert_msg)
    cv_m = cv_msg(0x0403, sign_cv_ecdsa(leaf_key, cv_content(th_cert)))
    th_pre = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m)
    fin_m = hs_msg(20, hmac_mod.new(s_fin, th_pre, hashlib.sha256).digest())
    srv.send_stream(conn, sk, si, sseq, ee_msg + cert_msg + cv_m + fin_m)
    th_sfin = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m, fin_m)
    ck2, ci2, sk2, si2 = app_keys(sched, th_sfin)
    cseq2, sseq2 = [0], [0]
    c_hs_fin = expand_label(c_hs, b"finished", b"", 32)
    drain_client_finished(conn, ck, ci, cseq, c_hs, th_sfin)
    read_app_request(conn, ck2, ci2, cseq2)
    send_app(conn, sk2, si2, sseq2, HTTP_RESP)
    # authenticated close_notify: clean end
    conn.send(aead_enc(sk2, si2, sseq2[0], 21, b"\x01\x00"))
    try:
        conn.s.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    return "encclose-sent"

@attack("sm-truncmid")
def a_sm_truncmid(srv, conn, idx):
    ch = read_ch(conn)
    srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    conn.send(record(22, hs_msg(2, sh_body)))
    # partial encrypted record then TCP close (truncation)
    conn.send(record(23, os.urandom(40))[:25])
    try:
        conn.s.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    conn.close()
    return "truncated"

@attack("sm-oversize")
def a_sm_oversize(srv, conn, idx):
    ch = read_ch(conn)
    srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    conn.send(record(22, hs_msg(2, sh_body)))
    # record header claiming 20000 bytes, then close
    conn.send(b"\x17\x03\x03\x4e\x20" + os.urandom(100))
    try:
        conn.s.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    conn.close()
    return "oversize-sent"

@attack("sm-badmac")
def a_sm_badmac(srv, conn, idx):
    pki = srv.pki
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    ee_msg = hs_msg(8, build_ee())
    rec = aead_enc(sk, si, 0, 22, ee_msg)
    rec = bytearray(rec)
    rec[10] ^= 0x01  # flip a ciphertext bit -> MAC must fail
    conn.send(bytes(rec))
    time.sleep(0.3)
    return "badmac-sent"

@attack("base-frag64")
def a_base_frag64(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", frag=64)

@attack("base-coalesce")
def a_base_coalesce(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", coalesce=True)

@attack("base-noalpn")
def a_base_noalpn(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", ee=build_ee(alpn=False))

@attack("ip-literal")
def a_ip_literal(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_ip_leaf.der")),
                   load_pem_key(f"{FIX}/at_ip_leaf.key"), "ec")

@attack("staple-good")
def a_staple_good(srv, conn, idx):
    pki = srv.pki
    leaf = load_der(f"{FIX}/at_leaf.der")
    ocsp_der = build_ocsp(pki, leaf)
    chk = ocsp_selfcheck(ocsp_der, leaf, pki.int_der)
    print(f"[ocsp] good-staple selfcheck={chk}", flush=True)
    return do_full(srv, conn, leaf_chain(pki, leaf),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec",
                   staple_map={0: ocsp_der})

@attack("muststaple-good")
def a_muststaple_good(srv, conn, idx):
    pki = srv.pki
    leaf = load_der(f"{FIX}/at_ms_leaf.der")
    ocsp_der = build_ocsp(pki, leaf)
    chk = ocsp_selfcheck(ocsp_der, leaf, pki.int_der)
    print(f"[ocsp] muststaple selfcheck={chk}", flush=True)
    return do_full(srv, conn, leaf_chain(pki, leaf),
                   load_pem_key(f"{FIX}/at_ms_leaf.key"), "ec",
                   staple_map={0: ocsp_der})

def do_order_violation(srv, conn, kinds):
    # kinds: list like ["ee","cert","cert","fin"] selecting which messages
    # to actually send, in order. Anything but exactly
    # [ee,cert,cv,fin] must die closed on the client.
    pki = srv.pki
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    sseq = [0]
    ee_msg = hs_msg(8, build_ee())
    cert_body = build_cert(leaf_chain(pki, load_der(f"{FIX}/at_leaf.der")))
    cert_msg = hs_msg(11, cert_body)
    leaf_key = load_pem_key(f"{FIX}/at_leaf.key")
    th_cert = thash(ch["msg"], sh_msg, ee_msg, cert_msg)
    cv_m = cv_msg(0x0403, sign_cv_ecdsa(leaf_key, cv_content(th_cert)))
    th_pre = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m)
    fin_m = hs_msg(20, hmac_mod.new(s_fin, th_pre, hashlib.sha256).digest())
    parts = {"ee": ee_msg, "cert": cert_msg, "cv": cv_m, "fin": fin_m,
             "certreq": hs_msg(13, b"\x00\x00\x00\x02\x04\x03\x05\x03"),
             "sh2": hs_msg(2, sh_body)}
    stream = b"".join(parts[k] for k in kinds)
    srv.send_stream(conn, sk, si, sseq, stream)
    time.sleep(0.3)
    return "order-violation-sent:" + ",".join(kinds)

@attack("sm-certreq")
def a_sm_certreq(srv, conn, idx):
    return do_order_violation(srv, conn, ["ee", "certreq", "cert", "cv", "fin"])

@attack("sm-dupee")
def a_sm_dupee(srv, conn, idx):
    return do_order_violation(srv, conn, ["ee", "ee", "cert", "cv", "fin"])

@attack("sm-doublecert")
def a_sm_doublecert(srv, conn, idx):
    return do_order_violation(srv, conn, ["ee", "cert", "cert", "cv", "fin"])

@attack("sm-skipcv")
def a_sm_skipcv(srv, conn, idx):
    return do_order_violation(srv, conn, ["ee", "cert", "fin"])

@attack("sm-skipcert")
def a_sm_skipcert(srv, conn, idx):
    return do_order_violation(srv, conn, ["ee", "cv", "fin"])

@attack("sm-2ndsh")
def a_sm_2ndsh(srv, conn, idx):
    return do_order_violation(srv, conn, ["ee", "sh2", "cert", "cv", "fin"])

@attack("sm-ccs2")
def a_sm_ccs2(srv, conn, idx):
    return do_ccs_mut(srv, conn, b"\x01\x01")

@attack("sm-ccs0")
def a_sm_ccs0(srv, conn, idx):
    return do_ccs_mut(srv, conn, b"")

def do_ccs_mut(srv, conn, ccs_body):
    pki = srv.pki
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    sseq = [0]
    ee_msg = hs_msg(8, build_ee())
    # malformed middlebox-compat CCS before the flight
    conn.send(record(20, ccs_body))
    cert_body = build_cert(leaf_chain(pki, load_der(f"{FIX}/at_leaf.der")))
    cert_msg = hs_msg(11, cert_body)
    leaf_key = load_pem_key(f"{FIX}/at_leaf.key")
    th_cert = thash(ch["msg"], sh_msg, ee_msg, cert_msg)
    cv_m = cv_msg(0x0403, sign_cv_ecdsa(leaf_key, cv_content(th_cert)))
    th_pre = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m)
    fin_m = hs_msg(20, hmac_mod.new(s_fin, th_pre, hashlib.sha256).digest())
    srv.send_stream(conn, sk, si, sseq, ee_msg + cert_msg + cv_m + fin_m)
    time.sleep(0.3)
    return "ccsmut-sent"

@attack("sm-plainalert")
def a_sm_plainalert(srv, conn, idx):
    ch = read_ch(conn)
    srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    conn.send(record(22, hs_msg(2, sh_body)))
    conn.send(record(21, b"\x02\x28"))  # fatal handshake_failure, plaintext
    time.sleep(0.3)
    return "plainalert-sent"

@attack("psk-ok")
def a_psk_ok(srv, conn, idx):
    if idx == 0:
        return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                       load_pem_key(f"{FIX}/at_leaf.key"), "ec")
    return do_psk_accept(srv, conn, tamper_binder=False, wrong_keys=False,
                         send_cert=False)

@attack("psk-foreign")
def a_psk_foreign(srv, conn, idx):
    if idx == 0:
        return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                       load_pem_key(f"{FIX}/at_leaf.key"), "ec")
    ch = read_ch(conn)
    offer = ch["psk"]
    assert offer, "round2 must offer PSK"
    ticket = offer["ticket"]
    assert ticket in srv.tickets
    res_master, nonce, _, _ = srv.tickets[ticket]
    psk = expand_label(res_master, b"resumption", nonce, 32)
    early = hkdf_extract(ZEROS, psk)
    # select a FOREIGN identity the client never offered
    sh_body, _ = srv.common_sh(ch)
    # splice pre_shared_key ext (type 41, body = selected_identity u16 = 5)
    psk_ext = p16(41) + p16(2) + p16(5)
    # insert before closing ext length: rebuild SH
    sh_body = sh_body[:-0]  # noop
    # parse: body = ver(2) rnd(32) sid(1+n) suite(2) comp(1) extlen(2) exts
    o = 2 + 32
    sl = sh_body[o]; o += 1 + sl + 2 + 1
    el = u16(sh_body, o)
    exts = sh_body[o + 2:o + 2 + el] + psk_ext
    sh_body = sh_body[:o] + p16(len(exts)) + exts
    conn.send(record(22, hs_msg(2, sh_body)))
    time.sleep(0.3)
    return "foreign-identity-sent"

@attack("psk-certafter")
def a_psk_certafter(srv, conn, idx):
    if idx == 0:
        return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                       load_pem_key(f"{FIX}/at_leaf.key"), "ec")
    return do_psk_accept(srv, conn, send_cert=True)

@attack("psk-wrongkeys")
def a_psk_wrongkeys(srv, conn, idx):
    if idx == 0:
        return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                       load_pem_key(f"{FIX}/at_leaf.key"), "ec")
    return do_psk_accept(srv, conn, wrong_keys=True)

@attack("psk-tamper")
def a_psk_tamper(srv, conn, idx):
    # server IGNORES a (valid) offer -> full-handshake fallback must succeed
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec")

@attack("pin-change")
def a_pin_change(srv, conn, idx):
    # round 2 serves a DIFFERENT valid leaf key for the same host
    if idx == 0:
        return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                       load_pem_key(f"{FIX}/at_leaf.key"), "ec")
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_rsa_leaf.der")),
                   load_pem_key(f"{FIX}/at_rsa_leaf.key"), "rsa-pss")

def do_psk_accept(srv, conn, tamper_binder=False, wrong_keys=False,
                  send_cert=False):
    ch = read_ch(conn)
    offer = ch["psk"]
    assert offer and len(offer["binders"]) == 1
    ticket = offer["ticket"]
    assert ticket in srv.tickets, "unknown ticket"
    res_master, nonce, _, _ = srv.tickets[ticket]
    real_psk = expand_label(res_master, b"resumption", nonce, 32)
    use_psk = os.urandom(32) if wrong_keys else real_psk
    early = hkdf_extract(ZEROS, use_psk)
    # verify binder (against the REAL psk; log only). Full RFC 8446
    # §4.2.11.2 construction (proven vs RFC 8448 §4 vectors): binder key =
    # Derive-Secret(early, "res binder", "") i.e. 32-byte empty-hash
    # context, then the full Finished recipe with BaseKey := binder key.
    e_hash = hashlib.sha256(b"").digest()
    bkey = expand_label(early if not wrong_keys else hkdf_extract(ZEROS, real_psk),
                        b"res binder", e_hash, 32)
    fkey = expand_label(bkey, b"finished", b"", 32)
    assert len(offer["binders"][0]) == 32
    th1 = hashlib.sha256(ch["msg"][:-35]).digest()
    binder_ok = hmac_mod.compare_digest(
        hmac_mod.new(fkey, th1, hashlib.sha256).digest(), offer["binders"][0])
    print(f"[psk] binder valid={binder_ok} wrong_keys={wrong_keys}", flush=True)
    # SH with pre_shared_key accept (identity 0)
    psk_ext = p16(41) + p16(2) + p16(0)
    sh_body, _ = srv.common_sh(ch, extra_exts=psk_ext)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    derived = expand_label(early, b"derived", EMPTY_HASH, 32)
    shared_raw = srv.priv.exchange(X25519PublicKey.from_public_bytes(ch["ke"]))
    hs = hkdf_extract(derived, shared_raw)
    th_sh = thash(ch["msg"], sh_msg)
    c_hs = expand_label(hs, b"c hs traffic", th_sh, 32)
    s_hs = expand_label(hs, b"s hs traffic", th_sh, 32)
    ck = expand_label(c_hs, b"key", b"", 32); ci = expand_label(c_hs, b"iv", b"", 12)
    sk = expand_label(s_hs, b"key", b"", 32); si = expand_label(s_hs, b"iv", b"", 12)
    s_fin = expand_label(s_hs, b"finished", b"", 32)
    sseq, cseq = [0], [0]
    ee_msg = hs_msg(8, build_ee())
    stream = ee_msg
    if send_cert:
        cert_body = build_cert(leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")))
        stream += hs_msg(11, cert_body)
    sfin = hmac_mod.new(s_fin, thash(ch["msg"], sh_msg, ee_msg), hashlib.sha256).digest()
    # NOTE: transcript for sfin in abbreviated flight = CH, SH, EE only
    fin_m = hs_msg(20, sfin)
    srv.send_stream(conn, sk, si, sseq, stream + fin_m)
    if send_cert:
        time.sleep(0.3)
        return "cert-after-accept-sent"
    # app keys
    th_sfin = thash(ch["msg"], sh_msg, ee_msg, fin_m)
    # (compute manually: need master)
    d2 = expand_label(hs, b"derived", EMPTY_HASH, 32)
    master = hkdf_extract(d2, ZEROS)
    c_ap = expand_label(master, b"c ap traffic", th_sfin, 32)
    s_ap = expand_label(master, b"s ap traffic", th_sfin, 32)
    ck2 = expand_label(c_ap, b"key", b"", 32); ci2 = expand_label(c_ap, b"iv", b"", 12)
    sk2 = expand_label(s_ap, b"key", b"", 32); si2 = expand_label(s_ap, b"iv", b"", 12)
    cseq2, sseq2 = [0], [0]
    c_hs_fin = expand_label(c_hs, b"finished", b"", 32)
    drain_client_finished(conn, ck, ci, cseq, c_hs, th_sfin)
    req = read_app_request(conn, ck2, ci2, cseq2)
    assert b"GET" in req
    th_both = thash(ch["msg"], sh_msg, ee_msg, fin_m,
                    hs_msg(20, hmac_mod.new(c_hs_fin, th_sfin, hashlib.sha256).digest()))
    rm = expand_label(master, b"res master", th_both, 32)
    send_nst(conn, sk2, si2, sseq2, srv, rm)
    send_app(conn, sk2, si2, sseq2, HTTP_RESP)
    try:
        conn.s.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    return "psk-accepted"

@attack("base-rsa4096")
def a_base_rsa4096(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_evil_rsa4096.der")),
                   load_pem_key(f"{FIX}/at_evil_rsa4096.key"), "rsa-pss")

@attack("base-frag7")
def a_base_frag7(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", frag=7)

@attack("ee-empty")
def a_ee_empty(srv, conn, idx):
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", ee=p16(0))

@attack("cv-alg384-on-256")
def a_cv_alg384_on_256(srv, conn, idx):
    # valid P-256 ECDSA sig advertised as ecdsa_secp384r1_sha384: key-type gate
    return do_full(srv, conn, leaf_chain(srv.pki, load_der(f"{FIX}/at_leaf.der")),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec", cv_alg_override=0x0503)

@attack("cv-empty")
def a_cv_empty(srv, conn, idx):
    pki = srv.pki
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    sseq = [0]
    ee_msg = hs_msg(8, build_ee())
    cert_msg = hs_msg(11, build_cert(leaf_chain(pki, load_der(f"{FIX}/at_leaf.der"))))
    cv_m = hs_msg(15, p16(0x0403) + p16(0))  # zero-length signature
    srv.send_stream(conn, sk, si, sseq, ee_msg + cert_msg + cv_m)
    time.sleep(0.3)
    return "cvempty-sent"

@attack("ch-6cert")
def a_ch_6cert(srv, conn, idx):
    pki = srv.pki
    leaf = load_der(f"{FIX}/at_leaf.der")
    # six individually-valid certs: depth cap must fail the flight closed
    chain = [(leaf, b""), (pki.int_der, b""), (pki.root_der, b""),
             (leaf, b""), (pki.int_der, b""), (pki.root_der, b"")]
    return do_full(srv, conn, chain, load_pem_key(f"{FIX}/at_leaf.key"), "ec")

@attack("nc-bad")
def a_nc_bad(srv, conn, idx):
    # evil.example.io violates the NC permitted subtree (example.com).
    # CV is garbage — never reached: cert_verify must fail first (CAFLAGS).
    pki = srv.pki
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    sseq = [0]
    ee_msg = hs_msg(8, build_ee())
    chain = [(load_der(f"{FIX}/at_nc_bad.der"), b""),
             (load_der(f"{FIX}/at_nc_int.der"), b""),
             (pki.root_der, b"")]
    cert_msg = hs_msg(11, build_cert(chain))
    cv_m = cv_msg(0x0403, os.urandom(64))
    th_pre = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m)
    fin_m = hs_msg(20, hmac_mod.new(s_fin, th_pre, hashlib.sha256).digest())
    srv.send_stream(conn, sk, si, sseq, ee_msg + cert_msg + cv_m + fin_m)
    time.sleep(0.5)
    return "ncbad-sent"

@attack("san-over")
def a_san_over(srv, conn, idx):
    # 17 SANs overflow the table: parse must fail closed (never truncate).
    # CV garbage — never reached.
    pki = srv.pki
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    sseq = [0]
    ee_msg = hs_msg(8, build_ee())
    chain = [(load_der(f"{FIX}/at_san_over.der"), b""),
             (pki.int_der, b""), (pki.root_der, b"")]
    cert_msg = hs_msg(11, build_cert(chain))
    cv_m = cv_msg(0x0403, os.urandom(64))
    th_pre = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m)
    fin_m = hs_msg(20, hmac_mod.new(s_fin, th_pre, hashlib.sha256).digest())
    srv.send_stream(conn, sk, si, sseq, ee_msg + cert_msg + cv_m + fin_m)
    time.sleep(0.5)
    return "sanover-sent"

@attack("sm-encalert")
def a_sm_encalert(srv, conn, idx):
    # encrypted fatal alert mid-flight (real keys): must file as ALERT
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    conn.send(aead_enc(sk, si, 0, 21, b"\x02\x28"))
    time.sleep(0.3)
    return "encalert-sent"

@attack("sm-nstbad")
def a_sm_nstbad(srv, conn, idx):
    # valid handshake, then a malformed NewSessionTicket: must die PROTO
    pki = srv.pki
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    sseq, cseq = [0], [0]
    ee_msg = hs_msg(8, build_ee())
    cert_body = build_cert(leaf_chain(pki, load_der(f"{FIX}/at_leaf.der")))
    cert_msg = hs_msg(11, cert_body)
    leaf_key = load_pem_key(f"{FIX}/at_leaf.key")
    th_cert = thash(ch["msg"], sh_msg, ee_msg, cert_msg)
    cv_m = cv_msg(0x0403, sign_cv_ecdsa(leaf_key, cv_content(th_cert)))
    th_pre = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m)
    fin_m = hs_msg(20, hmac_mod.new(s_fin, th_pre, hashlib.sha256).digest())
    srv.send_stream(conn, sk, si, sseq, ee_msg + cert_msg + cv_m + fin_m)
    th_sfin = thash(ch["msg"], sh_msg, ee_msg, cert_msg, cv_m, fin_m)
    ck2, ci2, sk2, si2 = app_keys(sched, th_sfin)
    cseq2 = [0]
    c_hs_fin = expand_label(c_hs, b"finished", b"", 32)
    drain_client_finished(conn, ck, ci, cseq, c_hs, th_sfin)
    read_app_request(conn, ck2, ci2, cseq2)
    # type 4 (NST) with a 3-byte body: structurally malformed
    conn.send(aead_enc(sk2, si2, 0, 22, hs_msg(4, b"\x00\x01\x02")))
    time.sleep(0.3)
    return "nstbad-sent"

@attack("sm-appdata-early")
def a_sm_appdata_early(srv, conn, idx):
    # application data where the handshake flight belongs (valid MAC!)
    ch = read_ch(conn)
    shared, sched = srv.derive_hs(ch["ke"])
    sh_body, _ = srv.common_sh(ch)
    sh_msg = hs_msg(2, sh_body)
    conn.send(record(22, sh_msg))
    th1 = thash(ch["msg"], sh_msg)
    c_hs, s_hs, ck, ci, sk, si, s_fin = flight_keys(sched, th1)
    conn.send(aead_enc(sk, si, 0, 23, b"HTTP/1.1 200 OK\r\n\r\n"))
    time.sleep(0.3)
    return "appdata-early-sent"

@attack("staple-sha256")
def a_staple_sha256(srv, conn, idx):
    pki = srv.pki
    leaf = load_der(f"{FIX}/at_leaf.der")
    ocsp_der = build_ocsp(pki, leaf, algo="sha256")
    chk = ocsp_selfcheck(ocsp_der, leaf, pki.int_der, algo="sha256")
    print(f"[ocsp] sha256-staple selfcheck={chk}", flush=True)
    return do_full(srv, conn, leaf_chain(pki, leaf),
                   load_pem_key(f"{FIX}/at_leaf.key"), "ec",
                   staple_map={0: ocsp_der})

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=False, default=0)
    ap.add_argument("--attack", required=False, default="list")
    ap.add_argument("--conns", type=int, default=1)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.list or a.attack == "list":
        print("\n".join(sorted(ATK)))
        return
    assert a.port, "--port required"
    assert a.attack in ATK, f"unknown attack {a.attack}"
    pki = PKI()
    srv = Srv(pki)
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((HOST, a.port))
    ls.listen(4)
    ls.settimeout(60)
    print(f"[evil] listening {HOST}:{a.port} attack={a.attack} conns={a.conns}",
          flush=True)
    served = 0
    while served < a.conns:
        try:
            cs, _ = ls.accept()
        except socket.timeout:
            break
        conn = Conn(cs)
        try:
            r = ATK[a.attack](srv, conn, served)
            print(f"[evil] conn{served}: {r}", flush=True)
        except Exception as e:
            import traceback
            print(f"[evil] conn{served}: server-error: {type(e).__name__}: {e}",
                  flush=True)
            traceback.print_exc()
        finally:
            conn.close()
        served += 1

if __name__ == "__main__":
    main()