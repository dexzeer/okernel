#!/bin/sh
# WebAssembly: every tests/web/js/wasm/t_*.js runs under the host browser (our engine) and its console output is compared
# with t_*.expected, which was produced by node (V8) from the same script:
#   powershell: & 'C:\Program Files\nodejs\node.exe' tests\web\js\wasm\node_run.js t_x.js > tests\web\js\wasm\t_x.expected
cd "$(dirname "$0")/../.." || exit 2
D=tests/web/js/wasm
PORT=${PORT:-8125}
(cd tests && python3 -m http.server $PORT --bind 127.0.0.1 >/dev/null 2>&1 & echo $! > /tmp/test_wasm_srv.pid)
sleep 1
fail=0
for t in $D/t_*.js; do
    n=$(basename "$t" .js)
    [ -f $D/$n.expected ] || { echo "SKIP $n (no expected output)"; continue; }
    printf '<!doctype html><html><body><script src="lib.js"></script><script src="%s.js"></script></body></html>' "$n" > $D/$n.gen.html
    timeout 600 ./build-host/wbrowse http://127.0.0.1:$PORT/web/js/wasm/$n.gen.html 640 400 /tmp/test_wasm_$n.ppm 20 2>&1 | grep '^\[js\] ' | sed 's/^\[js\] //' | grep -v '^load event:' > /tmp/test_wasm_$n.out
    if cmp -s /tmp/test_wasm_$n.out $D/$n.expected; then echo "PASS $n"; else
        echo "FAIL $n"; python3 tests/web/wasm_diff.py /tmp/test_wasm_$n.out $D/$n.expected; fail=1
    fi
    rm -f $D/$n.gen.html
done
kill "$(cat /tmp/test_wasm_srv.pid)" 2>/dev/null
[ $fail = 0 ] && echo "test_wasm: PASS" || echo "test_wasm: FAIL"
exit $fail
