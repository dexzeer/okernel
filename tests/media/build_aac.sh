#!/bin/sh
# build + run the AAC decoder tests (host, sanitizers): sh tests/media/build_aac.sh [vector names...]
#   FUZZ=n runs the damaged-stream fuzzer for n iterations
cd "$(dirname "$0")/../.." || exit 2
mkdir -p build-host
gcc -m32 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined -include stdlib.h \
    -o build-host/t_aac tests/media/test_aac.c src/media/aac_tables.c -lm || exit 1
ASAN_OPTIONS=detect_leaks=0 ./build-host/t_aac imdct | tail -n 1
names=${*:-$(cd tests/media/vec && ls aac_*.aac | sed 's/^aac_//; s/\.aac$//')}
for n in $names; do
    ASAN_OPTIONS=detect_leaks=0 ./build-host/t_aac tests/media/vec/aac_$n.aac tests/media/vec/aac_$n.f32 2>&1 | tail -n 1 | sed 's/^tests.media.vec.//'
done
if [ -n "$FUZZ" ]; then
    gcc -m32 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined \
        -o build-host/t_aac_fuzz tests/media/fuzz_aac.c src/media/aac.c src/media/aac_tables.c -lm || exit 1
    V=tests/media/vec/aac
    ASAN_OPTIONS=detect_leaks=0 ./build-host/t_aac_fuzz $FUZZ ${SEED:-1} $V"_music44.aac" $V"_trans44.aac" $V"_corr_ms.aac" $V"_tns.aac" $V"_mono44.aac" $V"_hibr.aac" $V"_pns.aac" $V"_anti_is.aac" 2>&1 | tail -n 5
fi
