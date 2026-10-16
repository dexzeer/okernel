#!/bin/sh
# Web API pages (tests/web/js/api_*.html and blob.html): each ends with "RESULT ALLPASS" on the console.
# Served over http://127.0.0.1 (workers and XHR need a real origin).
cd "$(dirname "$0")/../.." || exit 2
PORT=${PORT:-8124}
(cd tests && python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1 & echo $! > /tmp/test_webapi_srv.pid)
sleep 1
fail=0
for f in tests/web/js/blob.html tests/web/js/api_*.html; do
    p=$(basename "$f")
    [ -f "$f" ] || continue
    out=$(timeout 120 ./build-host/wbrowse http://127.0.0.1:$PORT/web/js/$p 640 400 /tmp/test_webapi_$p.ppm 8 2>&1)
    if ! printf '%s\n' "$out" | grep -q 'RESULT ALLPASS'; then echo "FAIL $p"; printf '%s\n' "$out" | grep -E "FAIL|RESULT|error" | head -10; fail=1; fi
done
kill "$(cat /tmp/test_webapi_srv.pid)" 2>/dev/null
[ $fail = 0 ] && echo "test_webapi: PASS" || echo "test_webapi: FAIL"
exit $fail
