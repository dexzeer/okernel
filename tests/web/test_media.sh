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
check mediamp4.html 'EV loadedmetadata' 'EV playing' 'play() resolved' 'EV ended' 'RESULT dur=3.0[0-9] size=320x180 frames=90 ' 'mp4/avc: \[probably\]'
check streams.html 'RESULT ALLPASS'
check mse.html 'SB video buffered=1 end=3.0 audio buffered=0 element buffered=1' 'sourceopen readyState=open' 'EV loadedmetadata' 'EV seeked' 'EV ended' 'RESULT dur=3.00 size=320x180 frames=1[0-9][0-9] seeked=true'
check msemp4.html 'sourceopen readyState=open' 'EV loadedmetadata' 'EV seeked' 'EV ended' 'RESULT dur=3.00 size=320x180 frames=1[0-9][0-9] seeked=true'
check mse_offset.html 'first: video 0.0-3.0 audio' 'second: video 0.0-6.0 audio -0.0-6.0 element 0.0-6.0' 'after audio remove: video 0.0-6.0 audio 1.5-6.0' 'window: video 0.0-6.0,9.0-10.0' 'reopened: open video 0.0-9.0' 'RESULT t=9.00 dur=9.00 frames=2[0-9][0-9]'
check mse_changetype.html 'mp4: video 0.1-3.1 audio 0.0-3.0' 'webm: video 3.0-6.0 audio 3.0-6.0' 'EV ended t=6.00' 'RESULT t=6.00 frames=90 size=320x180'
check adswap.html 'STAGE 0 ended dur=3.0[0-9] ' 'STAGE 1 ended dur=3.00' 'STAGE 2 ended' 'RESULT ALLSTAGES'
kill "$(cat /tmp/test_media_srv.pid)" 2>/dev/null
[ $fail = 0 ] && echo "test_media: PASS" || echo "test_media: FAIL"
exit $fail
