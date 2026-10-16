#!/bin/sh
# usage: sh tests/web/wasm_debug.sh t_random.js 'var DETAIL=1,FIRST_SEED=32,LAST_SEED=32'  -> prints what our engine prints
cd "$(dirname "$0")/../.." || exit 2
D=tests/web/js/wasm
PORT=${PORT:-8126}
(cd tests && python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1 & echo $! > /tmp/wasm_dbg_srv.pid)
sleep 1
printf '<!doctype html><html><body><script>%s</script><script src="lib.js"></script><script src="dis.js"></script><script src="%s"></script></body></html>' "$2" "$1" > $D/dbg.gen.html
OJS_NOBUDGET=1 timeout 800 ./build-host/wbrowse http://127.0.0.1:$PORT/web/js/wasm/dbg.gen.html 640 400 /tmp/wasm_dbg.ppm 20 2>&1 | grep '^\[js\] ' | sed 's/^\[js\] //' | grep -v '^load event:'
rm -f $D/dbg.gen.html
kill "$(cat /tmp/wasm_dbg_srv.pid)" 2>/dev/null
