#!/usr/bin/env python3
# Mint preloaded-pin test certs (cryptoholes follow-up): same SAN on two
# different existing keys (no new key material). The test preload table
# pins at_leaf's SPKI for "preload-test.example.com".
#   at_preload_leaf.der  SAN preload-test.example.com, at_leaf KEY   -> pin OK
#   at_preload_bad.der   SAN preload-test.example.com, at_rsa_leaf KEY -> PRELOAD fail
import datetime, os
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization

D = "tests/adversarial"
HOST = "preload-test.example.com"

def load_key(p):
    with open(p, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)

def load_cert(p):
    with open(p, "rb") as f:
        return x509.load_pem_x509_certificate(f.read())

_base = datetime.datetime.fromtimestamp(
    os.path.getmtime(f"{D}/at_leaf.der"), tz=datetime.timezone.utc)
now, far = _base, _base + datetime.timedelta(days=3000)
ca_cert = load_cert(f"{D}/at_int.pem")
ca_ski = ca_cert.extensions.get_extension_for_class(
    x509.SubjectKeyIdentifier).value.digest

def leaf(name, keyfile, rsa=False):
    key = load_key(f"{D}/{keyfile}")
    cert = (x509.CertificateBuilder()
            .subject_name(x509.Name([x509.NameAttribute(
                NameOID.COMMON_NAME, HOST)]))
            .issuer_name(ca_cert.subject)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(hours=1))
            .not_valid_after(far)
            .add_extension(x509.BasicConstraints(ca=False,
                                                 path_length=None), True)
            .add_extension(x509.SubjectAlternativeName(
                [x509.DNSName(HOST)]), False)
            .add_extension(x509.AuthorityKeyIdentifier(
                key_identifier=ca_ski, authority_cert_issuer=None,
                authority_cert_serial_number=None), False)
            .sign(load_key(f"{D}/at_int.key"), hashes.SHA256()))
    with open(f"{D}/{name}.der", "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.DER))
    print("wrote", name, flush=True)

leaf("at_preload_leaf", "at_leaf.key")
leaf("at_preload_bad", "at_rsa_leaf.key", rsa=True)
