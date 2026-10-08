#!/usr/bin/env python3
# Mint the fixtures for the 2026-10-08 crypto audit regressions
# (test_adversarial.c audit_section + MOCK_MS_FORGED_ISSUER). Same
# hermetic clock base as mint_nc.py (at_leaf.der mtime).
#
#   at_ncdot_xint.der   CA, excluded DNS:.bad.example.com (leading-dot form)
#   at_ncdot_xlf.der    leaf www.bad.example.com under it   -> CAFLAGS
#   at_ncdot_pint.der   CA, permitted DNS:.example.com
#   at_ncdot_pok.der    leaf evil.example.com under it      -> OK
#   at_ncdot_pbad.der   leaf example.com under it           -> CAFLAGS
#                       (leading dot = subdomains only)
#   at_dup_ctl.der      leaf re-encoded + re-signed, unchanged -> OK (control)
#   at_dup_eku.der      leaf with EKU twice (clientAuth, then serverAuth)
#                       -> parse fails (duplicate extension)
#   at_bc_junk.der      leaf whose BasicConstraints value has a trailing
#                       byte after the SEQUENCE -> parse fails
#   at_ms_self.*        self-signed Must-Staple leaf (pinned via the test
#                       trust slot)
#   at_evil_ca.*        a CA with the SAME subject name as at_ms_self, but
#                       its own key: the forged "issuer" an attacker appends
#                       to sign a "good" OCSP staple
#
#   python3 tests/adversarial/mint_audit.py
import datetime, os
from cryptography import x509
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

D = "tests/adversarial"

def load_key(p):
    with open(p, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)

def load_cert(p):
    with open(p, "rb") as f:
        return x509.load_pem_x509_certificate(f.read())

_base = datetime.datetime.fromtimestamp(
    os.path.getmtime(f"{D}/at_leaf.der"), tz=datetime.timezone.utc)
now, far = _base, _base + datetime.timedelta(days=3000)

def write(name, cert, key=None):
    with open(f"{D}/{name}.der", "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.DER))
    if key is not None:
        with open(f"{D}/{name}.pem", "wb") as f:
            f.write(cert.public_bytes(serialization.Encoding.PEM))
        with open(f"{D}/{name}.key", "wb") as f:
            f.write(key.private_bytes(serialization.Encoding.PEM,
                                      serialization.PrivateFormat.TraditionalOpenSSL,
                                      serialization.NoEncryption()))
    print("wrote", name, flush=True)

KU_CA = x509.KeyUsage(digital_signature=False, content_commitment=False,
                      key_encipherment=False, data_encipherment=False,
                      key_agreement=False, key_cert_sign=True, crl_sign=False,
                      encipher_only=False, decipher_only=False)
KU_LEAF = x509.KeyUsage(digital_signature=True, content_commitment=False,
                        key_encipherment=False, data_encipherment=False,
                        key_agreement=False, key_cert_sign=False, crl_sign=False,
                        encipher_only=False, decipher_only=False)

root_key = load_key(f"{D}/at_root.key")
root_cert = load_cert(f"{D}/at_root.pem")
int_key = load_key(f"{D}/at_int.key")
int_cert = load_cert(f"{D}/at_int.pem")

# ---- name constraints, leading-dot form ----
def nc_ca(name, cn, tag, permit=None, exclude=None):
    key = ec.generate_private_key(ec.SECP256R1())
    b = (x509.CertificateBuilder()
         .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)]))
         .issuer_name(root_cert.subject)
         .public_key(key.public_key())
         .serial_number(x509.random_serial_number())
         .not_valid_before(now - datetime.timedelta(hours=1))
         .not_valid_after(far)
         .add_extension(x509.BasicConstraints(ca=True, path_length=0), True)
         .add_extension(KU_CA, True)
         .add_extension(x509.SubjectKeyIdentifier(b"audit-ski-" + tag), False)
         .add_extension(x509.NameConstraints(
             [x509.DNSName(d) for d in permit] if permit else None,
             [x509.DNSName(d) for d in exclude] if exclude else None), True))
    cert = b.sign(root_key, hashes.SHA256())
    write(name, cert)
    return key, cert

def leaf(dns, ca_key, ca_cert, eku=None):
    key = ec.generate_private_key(ec.SECP256R1())
    ca_ski = ca_cert.extensions.get_extension_for_class(
        x509.SubjectKeyIdentifier).value.digest
    b = (x509.CertificateBuilder()
         .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, dns)]))
         .issuer_name(ca_cert.subject)
         .public_key(key.public_key())
         .serial_number(x509.random_serial_number())
         .not_valid_before(now - datetime.timedelta(hours=1))
         .not_valid_after(far)
         .add_extension(x509.BasicConstraints(ca=False, path_length=None), True)
         .add_extension(KU_LEAF, True)
         .add_extension(x509.SubjectAlternativeName([x509.DNSName(dns)]), False)
         .add_extension(x509.AuthorityKeyIdentifier(
             key_identifier=ca_ski, authority_cert_issuer=None,
             authority_cert_serial_number=None), False))
    if eku:
        b = b.add_extension(x509.ExtendedKeyUsage(eku), False)
    return key, b.sign(ca_key, hashes.SHA256())

k, c = nc_ca("at_ncdot_xint", "NC Dot Excluded", b"x0001", exclude=[".bad.example.com"])
write("at_ncdot_xlf", leaf("www.bad.example.com", k, c)[1])
k, c = nc_ca("at_ncdot_pint", "NC Dot Permitted", b"p0001", permit=[".example.com"])
write("at_ncdot_pok", leaf("evil.example.com", k, c)[1])
write("at_ncdot_pbad", leaf("example.com", k, c)[1])

# ---- raw DER surgery (duplicate extension, trailing junk) ----
def enc_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b

def tlv(tag, content):
    return bytes([tag]) + enc_len(len(content)) + content

def parse_tlv(b, i):
    tag = b[i]; i += 1
    l = b[i]; i += 1
    if l & 0x80:
        n = l & 0x7F
        l = int.from_bytes(b[i:i + n], "big"); i += n
    return tag, b[i:i + l], i + l

def kids(content):
    out, i = [], 0
    while i < len(content):
        tag, c, i = parse_tlv(content, i)
        out.append((tag, c))
    return out

ECDSA_SHA256 = tlv(0x30, tlv(0x06, bytes.fromhex("2a8648ce3d040302")))

def resign(tbs_fields, key):
    tbs = tlv(0x30, b"".join(tlv(t, c) for t, c in tbs_fields))
    sig = key.sign(tbs, ec.ECDSA(hashes.SHA256()))
    return tlv(0x30, tbs + ECDSA_SHA256 + tlv(0x03, b"\x00" + sig))

def mutate(cert, key, fn):
    _, tbs_content, _ = parse_tlv(cert.tbs_certificate_bytes, 0)
    fields = kids(tbs_content)
    tag, wrap = fields[-1]
    assert tag == 0xA3
    _, ext_seq, _ = parse_tlv(wrap, 0)
    exts = [tlv(t, c) for t, c in kids(ext_seq)]
    exts = fn(exts)
    fields[-1] = (0xA3, tlv(0x30, b"".join(exts)))
    return resign(fields, key)

OID_EKU = tlv(0x06, bytes.fromhex("551d25"))
OID_BC = tlv(0x06, bytes.fromhex("551d13"))
EKU_SERVER = tlv(0x30, OID_EKU + tlv(0x04, tlv(0x30, tlv(0x06, bytes.fromhex("2b06010505070301")))))

_, base = leaf("evil.example.com", int_key, int_cert, eku=[ExtendedKeyUsageOID.CLIENT_AUTH])
_, plain = leaf("evil.example.com", int_key, int_cert, eku=[ExtendedKeyUsageOID.SERVER_AUTH])
open(f"{D}/at_dup_ctl.der", "wb").write(mutate(plain, int_key, lambda e: e)); print("wrote at_dup_ctl")
open(f"{D}/at_dup_eku.der", "wb").write(mutate(base, int_key, lambda e: e + [EKU_SERVER])); print("wrote at_dup_eku")

def bc_junk(exts):
    out = []
    for e in exts:
        _, c, _ = parse_tlv(e, 0)
        parts = kids(c)
        if tlv(*parts[0]) == OID_BC:
            # value OCTET STRING: SEQUENCE {} + one trailing byte
            parts[-1] = (0x04, parts[-1][1] + b"\x00")
            e = tlv(0x30, b"".join(tlv(t, cc) for t, cc in parts))
        out.append(e)
    return out
open(f"{D}/at_bc_junk.der", "wb").write(mutate(plain, int_key, bc_junk)); print("wrote at_bc_junk")

# ---- Must-Staple self-signed leaf + same-name forged CA ----
NAME = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "evil.example.com")])
ms_key = ec.generate_private_key(ec.SECP256R1())
ms = (x509.CertificateBuilder()
      .subject_name(NAME).issuer_name(NAME)
      .public_key(ms_key.public_key())
      .serial_number(x509.random_serial_number())
      .not_valid_before(now - datetime.timedelta(hours=1))
      .not_valid_after(far)
      .add_extension(x509.BasicConstraints(ca=False, path_length=None), True)
      .add_extension(KU_LEAF, True)
      .add_extension(x509.SubjectAlternativeName([x509.DNSName("evil.example.com")]), False)
      .add_extension(x509.TLSFeature([x509.TLSFeatureType.status_request]), False)
      .sign(ms_key, hashes.SHA256()))
write("at_ms_self", ms, ms_key)
ev_key = ec.generate_private_key(ec.SECP256R1())
ev = (x509.CertificateBuilder()
      .subject_name(NAME).issuer_name(NAME)
      .public_key(ev_key.public_key())
      .serial_number(x509.random_serial_number())
      .not_valid_before(now - datetime.timedelta(hours=1))
      .not_valid_after(far)
      .add_extension(x509.BasicConstraints(ca=True, path_length=None), True)
      .add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False,
                                   key_encipherment=False, data_encipherment=False,
                                   key_agreement=False, key_cert_sign=True, crl_sign=False,
                                   encipher_only=False, decipher_only=False), True)
      .sign(ev_key, hashes.SHA256()))
write("at_evil_ca", ev, ev_key)
