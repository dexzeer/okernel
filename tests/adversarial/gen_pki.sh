#!/bin/bash
# Generates the ADVERSARIAL TEST PKI (private keys are TEST-ONLY fixtures —
# the production store never contains these). Outputs DER certs + PEM keys:
#   at_root   ECDSA P-256 self-signed root
#   at_int    ECDSA P-256 intermediate (signed by root)
#   at_leaf   ECDSA P-256 leaf, SAN: evil.example.com, *.evil.example.com
#   at_leaf_expired  same but expired (notAfter in the past)
#   at_rsa_leaf      RSA-2048 leaf (for RSA-PSS CV tests), SAN evil.example.com
#   at_p384_root/int/leaf  ECDSA P-384 chain (for 0x0503 CV tests), SAN evil.example.com
set -e
cd "$(dirname "$0")"

openssl ecparam -name prime256v1 -genkey -noout -out at_root.key 2>/dev/null
openssl req -new -x509 -key at_root.key -out at_root.pem -days 3650 \
  -subj "/CN=Adversarial Test Root" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign" 2>/dev/null

openssl ecparam -name prime256v1 -genkey -noout -out at_int.key 2>/dev/null
openssl req -new -key at_int.key -out at_int.csr -subj "/CN=Adversarial Test Intermediate" 2>/dev/null
openssl x509 -req -in at_int.csr -CA at_root.pem -CAkey at_root.key \
  -out at_int.pem -days 3650 -extfile <(printf "basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid") 2>/dev/null

openssl ecparam -name prime256v1 -genkey -noout -out at_leaf.key 2>/dev/null
openssl req -new -key at_leaf.key -out at_leaf.csr -subj "/CN=evil.example.com" 2>/dev/null
openssl x509 -req -in at_leaf.csr -CA at_int.pem -CAkey at_int.key \
  -out at_leaf.pem -days 3650 \
  -extfile <(printf "basicConstraints=CA:FALSE\nkeyUsage=digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:evil.example.com,DNS:*.evil.example.com") 2>/dev/null

# expired leaf: notAfter in the past via -days won't do negative; use faketime-free trick:
openssl ecparam -name prime256v1 -genkey -noout -out at_leaf_expired.key 2>/dev/null
openssl req -new -key at_leaf_expired.key -out at_leaf_expired.csr -subj "/CN=evil.example.com" 2>/dev/null
# openssl x509 can't backdate directly; generate with -days 1 then rely on the
# test setting "now" far in the future (2031) instead of a backdated cert.
openssl x509 -req -in at_leaf_expired.csr -CA at_int.pem -CAkey at_int.key \
  -out at_leaf_expired.pem -days 1 \
  -extfile <(printf "basicConstraints=CA:FALSE\nsubjectAltName=DNS:evil.example.com") 2>/dev/null

openssl genrsa -out at_rsa_leaf.key 2048 2>/dev/null
openssl req -new -key at_rsa_leaf.key -out at_rsa_leaf.csr -subj "/CN=evil.example.com" 2>/dev/null
openssl x509 -req -in at_rsa_leaf.csr -CA at_int.pem -CAkey at_int.key \
  -out at_rsa_leaf.pem -days 3650 \
  -extfile <(printf "basicConstraints=CA:FALSE\nkeyUsage=digitalSignature\nsubjectAltName=DNS:evil.example.com") 2>/dev/null

for n in at_root at_int at_leaf at_leaf_expired at_rsa_leaf; do
  openssl x509 -in $n.pem -outform DER -out $n.der
done
rm -f at_int.csr at_leaf.csr at_leaf_expired.csr at_rsa_leaf.csr

# P-384 chain (secp384r1 throughout — exercises the 0x0503 CV path end to end)
openssl ecparam -name secp384r1 -genkey -noout -out at_p384_root.key 2>/dev/null
openssl req -new -x509 -key at_p384_root.key -out at_p384_root.pem -days 3650 \
  -subj "/CN=Adversarial Test P-384 Root" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign" 2>/dev/null

openssl ecparam -name secp384r1 -genkey -noout -out at_p384_int.key 2>/dev/null
openssl req -new -key at_p384_int.key -out at_p384_int.csr -subj "/CN=Adversarial Test P-384 Intermediate" 2>/dev/null
openssl x509 -req -sha384 -in at_p384_int.csr -CA at_p384_root.pem -CAkey at_p384_root.key \
  -out at_p384_int.pem -days 3650 -extfile <(printf "basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid") 2>/dev/null

openssl ecparam -name secp384r1 -genkey -noout -out at_p384_leaf.key 2>/dev/null
openssl req -new -key at_p384_leaf.key -out at_p384_leaf.csr -subj "/CN=evil.example.com" 2>/dev/null
openssl x509 -req -sha384 -in at_p384_leaf.csr -CA at_p384_int.pem -CAkey at_p384_int.key \
  -out at_p384_leaf.pem -days 3650 \
  -extfile <(printf "basicConstraints=CA:FALSE\nkeyUsage=digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:evil.example.com,DNS:*.evil.example.com") 2>/dev/null

for n in at_p384_root at_p384_int at_p384_leaf; do
  openssl x509 -in $n.pem -outform DER -out $n.der
done
rm -f at_p384_int.csr at_p384_leaf.csr

# NEGATIVE fixtures (review 2026-09-10 #6 — must all be REJECTED):
#   at_ku_leaf   keyUsage WITHOUT digitalSignature (keyEncipherment only)
#   at_eku_leaf  EKU WITHOUT serverAuth (clientAuth only)
#   at_crit_leaf unknown CRITICAL extension (1.2.3.4.5.6) — parse must fail
# (all leaf certs signed by at_int, SAN evil.example.com so that ONLY the
# targeted check fails, never hostname)
openssl ecparam -name prime256v1 -genkey -noout -out at_ku_leaf.key 2>/dev/null
openssl req -new -key at_ku_leaf.key -out at_ku_leaf.csr -subj "/CN=evil.example.com" 2>/dev/null
openssl x509 -req -in at_ku_leaf.csr -CA at_int.pem -CAkey at_int.key \
  -out at_ku_leaf.pem -days 3650 \
  -extfile <(printf "basicConstraints=CA:FALSE\nkeyUsage=critical,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:evil.example.com") 2>/dev/null

openssl ecparam -name prime256v1 -genkey -noout -out at_eku_leaf.key 2>/dev/null
openssl req -new -key at_eku_leaf.key -out at_eku_leaf.csr -subj "/CN=evil.example.com" 2>/dev/null
openssl x509 -req -in at_eku_leaf.csr -CA at_int.pem -CAkey at_int.key \
  -out at_eku_leaf.pem -days 3650 \
  -extfile <(printf "basicConstraints=CA:FALSE\nkeyUsage=digitalSignature\nextendedKeyUsage=clientAuth\nsubjectAltName=DNS:evil.example.com") 2>/dev/null

openssl ecparam -name prime256v1 -genkey -noout -out at_crit_leaf.key 2>/dev/null
openssl req -new -key at_crit_leaf.key -out at_crit_leaf.csr -subj "/CN=evil.example.com" 2>/dev/null
openssl x509 -req -in at_crit_leaf.csr -CA at_int.pem -CAkey at_int.key \
  -out at_crit_leaf.pem -days 3650 \
  -extfile <(printf "basicConstraints=CA:FALSE\nkeyUsage=digitalSignature\nsubjectAltName=DNS:evil.example.com\n1.2.3.4.5.6=critical,DER:05:00") 2>/dev/null

for n in at_ku_leaf at_eku_leaf at_crit_leaf; do
  openssl x509 -in $n.pem -outform DER -out $n.der
done
rm -f at_ku_leaf.csr at_eku_leaf.csr at_crit_leaf.csr
echo "test PKI ready"
