#!/bin/bash
# Our TLS 1.2 client against openssl s_server: every suite with the key type it
# needs, a client-certificate request, and plain TLS 1.3 next to it.
# usage: tests/tls12_interop.sh [t_tlsa binary]   (run from the repo root)
T=${1:-build-host/t_tlsa}
D=$(mktemp -d)
P=45143
pass=0; fail=0
trap 'kill $SP 2>/dev/null; rm -rf "$D"' EXIT

pki() {                # pki <name> <key options...>: a root + a leaf for evil.example.com
    local n=$1; shift
    openssl req -x509 -newkey "$@" -nodes -keyout $D/$n-root.key -out $D/$n-root.pem -days 30 -subj "/CN=tls12 test root $n" \
        -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign 2>/dev/null
    openssl req -newkey "$@" -nodes -keyout $D/$n.key -out $D/$n.csr -subj "/CN=evil.example.com" 2>/dev/null
    printf 'subjectAltName=DNS:evil.example.com\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature\nextendedKeyUsage=serverAuth\n' > $D/$n.ext
    openssl x509 -req -in $D/$n.csr -CA $D/$n-root.pem -CAkey $D/$n-root.key -CAcreateserial -days 30 -out $D/$n.pem -extfile $D/$n.ext 2>/dev/null
    openssl x509 -in $D/$n-root.pem -outform DER -out $D/$n-root.der
}
pki rsa rsa:2048
pki p256 ec -pkeyopt ec_paramgen_curve:prime256v1
pki p384 ec -pkeyopt ec_paramgen_curve:secp384r1

# run <name> <pki> <line the client must log> <s_server options...>
run() {
    local name=$1 k=$2 want=$3; shift 3
    openssl s_server -accept $P -cert $D/$k.pem -key $D/$k.key -cert_chain $D/$k-root.pem -www -naccept 1 "$@" > $D/srv.log 2>&1 &
    SP=$!
    sleep 0.3
    TLSA_ROOT=$D/$k-root.der timeout 30 "$T" $P > $D/cli.log 2>&1
    kill $SP 2>/dev/null
    wait $SP 2>/dev/null
    if grep -qE "$want" $D/cli.log && grep -qE 'run done: n=[1-9]' $D/cli.log && grep -q 'HTTP/1.0 200' $D/cli.log; then
        echo "PASS $name"; pass=$((pass+1))
    else
        echo "FAIL $name"; fail=$((fail+1))
        grep -hE 'run done|\[tls\]|error' $D/cli.log $D/srv.log | head -6 | cut -c1-150
    fi
}

G="-groups X25519:P-256:P-384"
run "ECDHE-RSA-CHACHA20-POLY1305"     rsa  "suite cca8" -tls1_2 $G -cipher ECDHE-RSA-CHACHA20-POLY1305
run "ECDHE-RSA-AES128-GCM-SHA256"     rsa  "suite c02f" -tls1_2 $G -cipher ECDHE-RSA-AES128-GCM-SHA256
run "ECDHE-RSA-AES256-GCM-SHA384"     rsa  "suite c030" -tls1_2 $G -cipher ECDHE-RSA-AES256-GCM-SHA384
run "ECDHE-ECDSA-CHACHA20 (P-256)"    p256 "suite cca9" -tls1_2 $G -cipher ECDHE-ECDSA-CHACHA20-POLY1305
run "ECDHE-ECDSA-AES128-GCM (P-256)"  p256 "suite c02b" -tls1_2 $G -cipher ECDHE-ECDSA-AES128-GCM-SHA256
run "ECDHE-ECDSA-AES256-GCM (P-256)"  p256 "suite c02c" -tls1_2 $G -cipher ECDHE-ECDSA-AES256-GCM-SHA384
run "ECDHE-ECDSA-AES256-GCM (P-384)"  p384 "suite c02c" -tls1_2 $G -cipher ECDHE-ECDSA-AES256-GCM-SHA384
run "ECDHE-ECDSA-AES128-GCM (P-384)"  p384 "suite c02b" -tls1_2 $G -cipher ECDHE-ECDSA-AES128-GCM-SHA256
run "client certificate requested"    rsa  "suite c02f" -tls1_2 $G -cipher ECDHE-RSA-AES128-GCM-SHA256 -verify 1
run "RSA-PSS only"                    rsa  "suite c02f" -tls1_2 $G -cipher ECDHE-RSA-AES128-GCM-SHA256 -sigalgs rsa_pss_rsae_sha256
run "RSA PKCS#1 SHA-512"              rsa  "suite c02f" -tls1_2 $G -cipher ECDHE-RSA-AES128-GCM-SHA256 -sigalgs RSA+SHA512
run "P-256 key exchange (RSA)"        rsa  "suite c02f group 0017" -tls1_2 -groups P-256 -cipher ECDHE-RSA-AES128-GCM-SHA256
run "P-384 key exchange (RSA)"        rsa  "suite c030 group 0018" -tls1_2 -groups P-384 -cipher ECDHE-RSA-AES256-GCM-SHA384
run "P-256 key exchange (ECDSA)"      p256 "suite c02b group 0017" -tls1_2 -groups P-256 -cipher ECDHE-ECDSA-AES128-GCM-SHA256
run "P-384 key exchange (ECDSA)"      p384 "suite cca9 group 0018" -tls1_2 -groups P-384 -cipher ECDHE-ECDSA-CHACHA20-POLY1305
run "TLS 1.3 still preferred"         rsa  "chain verified" $G; grep -q "TLS 1.2 up" $D/cli.log && { echo "FAIL that was 1.2"; fail=$((fail+1)); }

echo "tls12 interop: $pass passed, $fail failed"
[ $fail = 0 ]
