#!/bin/sh
# <video>/MediaSource through the web engine on the host (no sound card: the tick clock).
# Needs the vectors in tests/media/vec (tests/media/gen_*.py) and build-host/wbrowse (tests/web/build.sh).
cd "$(dirname "$0")/../.." || exit 2
PORT=${PORT:-8123}
(cd tests && python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1 & echo $! > /tmp/test_media_srv.pid)
sleep 1
fail=0
check() {  # page pattern...
    page=$1; shift
    out=$(timeout 120 ./build-host/wbrowse http://127.0.0.1:$PORT/web/js/$page 640 400 /tmp/test_media_$page.ppm 25 2>&1)
    for pat in "$@"; do
        if ! printf '%s\n' "$out" | grep -q -- "$pat"; then echo "FAIL $page: no '$pat'"; printf '%s\n' "$out" | tail -15; fail=1; fi
    done
}
check media.html 'EV loadedmetadata' 'EV playing' 'play() resolved' 'EV ended' 'RESULT dur=3.01 size=320x180 frames=90 ' 'probably'
check streams.html 'RESULT ALLPASS'
check mse.html 'SB video buffered=1 end=3.0 audio buffered=0 element buffered=1' 'sourceopen readyState=open' 'EV loadedmetadata' 'EV seeked' 'EV ended' 'RESULT dur=3.00 size=320x180 frames=1[0-9][0-9] seeked=true'
kill "$(cat /tmp/test_media_srv.pid)" 2>/dev/null
[ $fail = 0 ] && echo "test_media: PASS" || echo "test_media: FAIL"
exit $fail
