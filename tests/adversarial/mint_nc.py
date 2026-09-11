#!/usr/bin/env python3
# Mint name-constraints test chains (P2 review round): intermediate CAs with
# permitted/excluded DNS subtrees + leaves inside/outside them. All signed
# under at_int (whose key we hold), anchored at at_root via the test trust
# hook. Only dNSName constraints (the implemented scope).
#   at_nc_int.der   CA, permitted DNS:example.com
#   at_nc_ok.der    leaf www.example.com under nc_int      -> must verify
#   at_nc_bad.der   leaf evil.example.io under nc_int      -> CAFLAGS
#   at_nc_xint.der  CA, excluded DNS:example.com
#   at_nc_xlf.der   leaf www.example.com under nc_xint     -> CAFLAGS
import datetime
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

# NOTE: intermediates hang off at_root (NOT at_int: at_int carries
# pathlen:0 and may not issue sub-CAs — using it here would fail pathlen,
# not name constraints).
int_key = load_key(f"{D}/at_root.key")
int_cert = load_cert(f"{D}/at_root.pem")
now = datetime.datetime.now(datetime.timezone.utc)
far = now + datetime.timedelta(days=3000)

def ca(name, cn, ski_tag, permit=None, exclude=None):
    key = ec.generate_private_key(ec.SECP256R1())
    # Explicit SKI (fixed bytes — at_root carries no SKI to reference, and
    # fixed tags make the AKI-match assertions exact).
    ski = b"nc-ski-" + ski_tag
    b = (x509.CertificateBuilder()
         .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)]))
         .issuer_name(int_cert.subject)
         .public_key(key.public_key())
         .serial_number(x509.random_serial_number())
         .not_valid_before(now - datetime.timedelta(hours=1))
         .not_valid_after(far)
         .add_extension(x509.BasicConstraints(ca=True, path_length=0), True)
         .add_extension(x509.KeyUsage(digital_signature=False,
                                      content_commitment=False,
                                      key_encipherment=False,
                                      data_encipherment=False,
                                      key_agreement=False, key_cert_sign=True,
                                      crl_sign=False, encipher_only=False,
                                      decipher_only=False), True)
         .add_extension(x509.SubjectKeyIdentifier(ski), False))
    nc_permit = [x509.DNSName(d) for d in (permit or [])]
    nc_exclude = [x509.DNSName(d) for d in (exclude or [])]
    if nc_permit or nc_exclude:
        b = b.add_extension(x509.NameConstraints(nc_permit or None,
                                                 nc_exclude or None), True)
    cert = b.sign(int_key, hashes.SHA256())
    with open(f"{D}/{name}.der", "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.DER))
    print("wrote", name, flush=True)
    return key, cert

def leaf(name, dns, ca_key, ca_cert):
    key = ec.generate_private_key(ec.SECP256R1())
    ca_ski = ca_cert.extensions.get_extension_for_class(
        x509.SubjectKeyIdentifier).value.digest
    cert = (x509.CertificateBuilder()
            .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,
                                                        dns)]))
            .issuer_name(ca_cert.subject)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(hours=1))
            .not_valid_after(far)
            .add_extension(x509.BasicConstraints(ca=False,
                                                 path_length=None), True)
            .add_extension(x509.KeyUsage(digital_signature=True,
                                         content_commitment=False,
                                         key_encipherment=False,
                                         data_encipherment=False,
                                         key_agreement=False,
                                         key_cert_sign=False, crl_sign=False,
                                         encipher_only=False,
                                         decipher_only=False), True)
            .add_extension(x509.SubjectAlternativeName(
                [x509.DNSName(dns)]), False)
            .add_extension(x509.AuthorityKeyIdentifier(
                key_identifier=ca_ski, authority_cert_issuer=None,
                authority_cert_serial_number=None), False)
            .sign(ca_key, hashes.SHA256()))
    with open(f"{D}/{name}.der", "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.DER))
    print("wrote", name, flush=True)

_, nc = ca("at_nc_int", "NC Test Intermediate", b"int00000000000001",
           permit=["example.com"])
leaf("at_nc_ok", "www.example.com", _, nc)
leaf("at_nc_bad", "evil.example.io", _, nc)
_, ncx = ca("at_nc_xint", "NC Excluded Intermediate", b"xint0000000000001",
            exclude=["example.com"])
leaf("at_nc_xlf", "www.example.com", _, ncx)
