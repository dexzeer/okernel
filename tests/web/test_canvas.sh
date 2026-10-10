#!/bin/sh
# <canvas> 2D through the web engine on the host: the scene pages must run without script errors, and
# the C canvas library's own scene (tests/web/test_canvas.c) must produce a PNG.
# Visual comparison with a browser: python3 tests/web/canvas_compare.py tests/web/js/canvas_scene.html
cd "$(dirname "$0")/../.." || exit 2
fail=0
for p in canvas_scene.html canvas_scene2.html canvas_text.html canvas_anim.html; do
    out=$(timeout 120 ./build-host/wbrowse tests/web/js/$p 500 360 /tmp/test_canvas_$p.ppm 5 2>&1)
    if { [ "$p" != canvas_anim.html ] && ! printf '%s\n' "$out" | grep -q 'RESULT'; } || printf '%s\n' "$out" | grep -q '\[js\] error'; then echo "FAIL $p"; printf '%s\n' "$out" | tail -5; fail=1; fi
done
out=$(timeout 120 ./build-host/wbrowse tests/web/js/canvas_scene2.html 500 360 /tmp/test_canvas_back.ppm 5 2>&1)
printf '%s\n' "$out" | grep -q 'RESULT back=60,60,128,255,66,60,128,255,60,66,128,255,66,66,128,255' || { echo "FAIL getImageData/putImageData round trip"; fail=1; }
[ $fail = 0 ] && echo "test_canvas: PASS" || echo "test_canvas: FAIL"
exit $fail
