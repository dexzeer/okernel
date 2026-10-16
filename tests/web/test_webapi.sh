#!/bin/sh
# Web API pages (tests/web/js/api_*.html and blob.html): each ends with "RESULT ALLPASS" on the console.
cd "$(dirname "$0")/../.." || exit 2
fail=0
for p in blob.html api_*.html; do
    [ -f tests/web/js/$p ] || continue
    out=$(timeout 120 ./build-host/wbrowse tests/web/js/$p 640 400 /tmp/test_webapi_$p.ppm 6 2>&1)
    if ! printf '%s\n' "$out" | grep -q 'RESULT ALLPASS'; then echo "FAIL $p"; printf '%s\n' "$out" | grep -E "FAIL|RESULT|error" | head -10; fail=1; fi
done
[ $fail = 0 ] && echo "test_webapi: PASS" || echo "test_webapi: FAIL"
exit $fail
