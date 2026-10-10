#!/bin/sh
# build + run the H.264 decoder test (host, sanitizers): sh tests/media/build_h264.sh [vector names...]
cd "$(dirname "$0")/../.." || exit 2
mkdir -p build-host
gcc -m32 -O1 -g -DH264_TRACE=${TRACE:-1} -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined -o build-host/t_h264 \
    tests/media/test_h264.c src/media/h264.c src/media/h264_slice.c src/media/h264_recon.c src/media/h264_cabac.c \
    src/media/h264_inter.c src/media/h264_lf.c src/media/h264_tables.c || exit 1
names=${*:-$(cd tests/media/vec && ls h264_*.h264 | sed 's/^h264_//; s/\.h264$//')}
fail=0
for n in $names; do
    printf '%-16s ' "$n"
    ASAN_OPTIONS=detect_leaks=0 ./build-host/t_h264 tests/media/vec/h264_$n.h264 tests/media/vec/h264_$n.yuv 2>&1 | head -3 | tr '\n' ' '
    echo
done
