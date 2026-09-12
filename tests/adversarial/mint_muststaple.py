#!/usr/bin/env python3
# Mint Must-Staple test leaf (RFC 7633): TLSFeature status_request(5),
# SAN evil.example.com, signed by at_int. Chains hang off the frozen
# leaf-mtime base like mint_nc.py (hermetic — never rots the suite).
#   at_ms_leaf.der  Must-Staple leaf -> unstapled must FAIL, stapled OK
import datetime, os
from cryptography import x509
from cryptography.x509.oid import NameOID
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

ca_key = load_key(f"{D}/at_int.key")
ca_cert = load_cert(f"{D}/at_int.pem")
ca_ski = ca_cert.extensions.get_extension_for_class(
    x509.SubjectKeyIdentifier).value.digest
key = ec.generate_private_key(ec.SECP256R1())
cert = (x509.CertificateBuilder()
        .subject_name(x509.Name([x509.NameAttribute(
            NameOID.COMMON_NAME, "evil.example.com")]))
        .issuer_name(ca_cert.subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(hours=1))
        .not_valid_after(far)
        .add_extension(x509.BasicConstraints(ca=False,
                                             path_length=None), True)
        .add_extension(x509.SubjectAlternativeName(
            [x509.DNSName("evil.example.com")]), False)
        .add_extension(x509.AuthorityKeyIdentifier(
            key_identifier=ca_ski, authority_cert_issuer=None,
            authority_cert_serial_number=None), False)
        .add_extension(x509.TLSFeature(
            [x509.TLSFeatureType.status_request]), False)
        .sign(ca_key, hashes.SHA256()))
der = cert.public_bytes(serialization.Encoding.DER)
with open(f"{D}/at_ms_leaf.key", "wb") as f:
    f.write(key.private_bytes(serialization.Encoding.PEM,
                              serialization.PrivateFormat.TraditionalOpenSSL,
                              serialization.NoEncryption()))
with open(f"{D}/at_ms_leaf.der", "wb") as f:
    f.write(der)
with open(f"{D}/at_ms_leaf.pem", "wb") as f:
    f.write(cert.public_bytes(serialization.Encoding.PEM))
print("wrote at_ms_leaf", flush=True)
