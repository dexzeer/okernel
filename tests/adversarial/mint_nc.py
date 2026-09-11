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
# Hermetic clocks (cryptoholes follow-up, 2026-09-11): the suite validates
# at leaf-mtime + 2h (adv_clocks), so mint relative to that SAME frozen
# base — never wall-clock now. Wall-minted notBefore drifts past the test
# clock within a day and reds every validity check (bisected: regen at
# wall-now broke the pre-existing NC tests). notAfter stays far-future.
import os as _os
_base = datetime.datetime.fromtimestamp(
    _os.path.getmtime(f"{D}/at_leaf.der"), tz=datetime.timezone.utc)
now = _base
far = _base + datetime.timedelta(days=3000)

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

# --- cryptoholes fail-closed fixtures (must FAIL verification) ---
import ipaddress

# 1. Overflow: 5 permitted DNS subtrees (parser cap X509_MAX_NC=4).
_, ncover = ca("at_nc_over", "NC Overflow Intermediate", b"over00000000000001",
               permit=["a.example.com", "b.example.com", "c.example.com",
                       "d.example.com", "e.example.com"])
leaf("at_nc_over_leaf", "www.a.example.com", _, ncover)

# 2. Unsupported name form: directoryName permitted subtree.
b2 = (x509.CertificateBuilder()
      .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,
                                                  "NC DirName Intermediate")]))
      .issuer_name(int_cert.subject)
      .public_key(ec.generate_private_key(ec.SECP256R1()).public_key())
      .serial_number(x509.random_serial_number())
      .not_valid_before(now - datetime.timedelta(hours=1))
      .not_valid_after(far)
      .add_extension(x509.BasicConstraints(ca=True, path_length=0), True)
      .add_extension(x509.SubjectKeyIdentifier(b"nc-ski-dir00000001"), False)
      .add_extension(x509.NameConstraints(
          permitted_subtrees=[x509.DirectoryName(x509.Name(
              [x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Constrained")]))],
          excluded_subtrees=None), True)
      .sign(int_key, hashes.SHA256()))
with open(f"{D}/at_nc_dir.der", "wb") as f:
    f.write(b2.public_bytes(serialization.Encoding.DER))
print("wrote at_nc_dir", flush=True)
leaf("at_nc_dir_leaf", "www.example.com", int_key,
     x509.load_der_x509_certificate(open(f"{D}/at_nc_dir.der", "rb").read()))

# 3. Unsupported address form: IPv6 excluded subtree.
_, ncv6 = ca("at_nc_ip6", "NC IPv6 Intermediate", b"ip600000000000001",
             exclude=["example.com"])
# (cryptography takes DNSName strs above; build the v6 variant explicitly)
b3 = (x509.CertificateBuilder()
      .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,
                                                  "NC IPv6 Intermediate")]))
      .issuer_name(int_cert.subject)
      .public_key(ec.generate_private_key(ec.SECP256R1()).public_key())
      .serial_number(x509.random_serial_number())
      .not_valid_before(now - datetime.timedelta(hours=1))
      .not_valid_after(far)
      .add_extension(x509.BasicConstraints(ca=True, path_length=0), True)
      .add_extension(x509.SubjectKeyIdentifier(b"nc-ski-ip60000001"), False)
      .add_extension(x509.NameConstraints(
          permitted_subtrees=None,
          excluded_subtrees=[x509.IPAddress(
              ipaddress.IPv6Network("2001:db8::/32"))]), True)
      .sign(int_key, hashes.SHA256()))
with open(f"{D}/at_nc_ip6.der", "wb") as f:
    f.write(b3.public_bytes(serialization.Encoding.DER))
print("wrote at_nc_ip6", flush=True)
leaf("at_nc_ip6_leaf", "www.example.com", int_key,
     x509.load_der_x509_certificate(open(f"{D}/at_nc_ip6.der", "rb").read()))

# 4. SAN overflow: leaf with 17 DNS SANs (parser cap X509_MAX_SAN=16),
#    signed by at_int (pathlen:0 allows leaves, not sub-CAs).
at_int_key = load_key(f"{D}/at_int.key")
at_int_cert = load_cert(f"{D}/at_int.pem")
at_int_ski = at_int_cert.extensions.get_extension_for_class(
    x509.SubjectKeyIdentifier).value.digest
san17 = [x509.DNSName(f"h{i}.example.com") for i in range(17)]
leaf17_key = ec.generate_private_key(ec.SECP256R1())
c17 = (x509.CertificateBuilder()
       .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,
                                                   "h0.example.com")]))
       .issuer_name(at_int_cert.subject)
       .public_key(leaf17_key.public_key())
       .serial_number(x509.random_serial_number())
       .not_valid_before(now - datetime.timedelta(hours=1))
       .not_valid_after(far)
       .add_extension(x509.BasicConstraints(ca=False, path_length=None), True)
       .add_extension(x509.SubjectAlternativeName(san17), False)
       .add_extension(x509.AuthorityKeyIdentifier(
           key_identifier=at_int_ski, authority_cert_issuer=None,
           authority_cert_serial_number=None), False)
       .sign(at_int_key, hashes.SHA256()))
with open(f"{D}/at_san_over.der", "wb") as f:
    f.write(c17.public_bytes(serialization.Encoding.DER))
print("wrote at_san_over", flush=True)
