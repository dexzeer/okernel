#!/bin/sh
# Mutation fuzz of the WebAssembly decoder/interpreter under the host browser. For the sanitizer build:
#   SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined" sh tests/web/build.sh && sh tests/web/wasm_fuzz.sh
# (ASan's fake stacks break the engine's stack-depth check: the script sets detect_stack_use_after_return=0; rebuild normally afterwards). Prints one JSON line of counters; any sanitizer report or crash is a failure.
cd "$(dirname "$0")/../.." || exit 2
D=tests/web/js/wasm
PORT=${PORT:-8127}
(cd tests && python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1 & echo $! > /tmp/wasm_fuzz_srv.pid)
sleep 1
printf '<!doctype html><html><body><script>var FIRST_SEED=1,LAST_SEED=0;%s</script><script src="lib.js"></script><script src="t_random.js"></script><script src="t_fuzz.js"></script></body></html>' "$1" > $D/fuzz.gen.html
ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=0 timeout 1500 ./build-host/wbrowse http://127.0.0.1:$PORT/web/js/wasm/fuzz.gen.html 640 400 /tmp/wasm_fuzz.ppm ${WAIT:-120} > /tmp/wasm_fuzz.log 2>&1
rc=$?
rm -f $D/fuzz.gen.html
kill "$(cat /tmp/wasm_fuzz_srv.pid)" 2>/dev/null
grep -E "^\[js\] fuzz|ERROR: AddressSanitizer|runtime error" /tmp/wasm_fuzz.log | head -5
echo "rc=$rc"
