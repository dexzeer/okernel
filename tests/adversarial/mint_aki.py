#!/usr/bin/env python3
# Mint AKI test leaves (review #10): same issuer (at_int), same SAN, KU/EKU
# clean — differing ONLY in AuthorityKeyIdentifier, to prove chain binding
# is by KEY, not by name.
#   at_aki_ok.der  AKI = at_int's real SKI  -> must verify
#   at_aki_bad.der AKI = 16 garbage bytes   -> must fail (CV_ERR_CHAIN)
# Signed with at_int.key (ECDSA P-256). Requires python cryptography.
import sys, datetime
from cryptography import x509
from cryptography.x509.oid import NameOID, ExtensionOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

D = "tests/adversarial"

def load_key(p):
    with open(p, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)

def load_cert(p):
    with open(p, "rb") as f:
        return x509.load_pem_x509_certificate(f.read())

int_key = load_key(f"{D}/at_int.key")
int_cert = load_cert(f"{D}/at_int.pem")
ski = int_cert.extensions.get_extension_for_oid(
    ExtensionOID.SUBJECT_KEY_IDENTIFIER).value.digest
print("at_int SKI:", ski.hex(), flush=True)

now = datetime.datetime.now(datetime.timezone.utc)
for tag, aki_bytes in (("ok", ski), ("bad", bytes([0xAA]) * 16)):
    leaf_key = ec.generate_private_key(ec.SECP256R1())
    leaf = (x509.CertificateBuilder()
            .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,
                                                        "evil.example.com")]))
            .issuer_name(int_cert.subject)
            .public_key(leaf_key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(hours=1))
            .not_valid_after(now + datetime.timedelta(days=3650))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None),
                           critical=True)
            .add_extension(x509.KeyUsage(digital_signature=True,
                                         content_commitment=False,
                                         key_encipherment=False,
                                         data_encipherment=False,
                                         key_agreement=False,
                                         key_cert_sign=False,
                                         crl_sign=False,
                                         encipher_only=False,
                                         decipher_only=False),
                           critical=True)
            .add_extension(x509.SubjectAlternativeName(
                [x509.DNSName("evil.example.com")]), critical=False)
            .add_extension(x509.AuthorityKeyIdentifier(
                key_identifier=aki_bytes, authority_cert_issuer=None,
                authority_cert_serial_number=None), critical=False)
            .sign(int_key, hashes.SHA256()))
    with open(f"{D}/at_aki_{tag}.der", "wb") as f:
        f.write(leaf.public_bytes(serialization.Encoding.DER))
    print(f"wrote at_aki_{tag}.der", flush=True)
