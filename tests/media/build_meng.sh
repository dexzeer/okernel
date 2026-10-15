#!/bin/sh
# build + run the media engine test (host, sanitizers): sh tests/media/build_meng.sh
cd "$(dirname "$0")/../.." || exit 2
mkdir -p build-host
M=src/media
gcc -m32 -O1 -g -msse2 -fsanitize=address,undefined -o build-host/t_meng tests/media/test_meng.c $M/meng.c $M/webm.c $M/mp4.c \
    $M/aac.c $M/aac_tables.c $M/resample.c $M/h264.c $M/h264_slice.c $M/h264_recon.c $M/h264_cabac.c $M/h264_inter.c $M/h264_lf.c $M/h264_tables.c \
    $M/opus_dec.c $M/opus_packet.c $M/celt.c $M/celt_tables.c $M/ec.c $M/vp9.c $M/vp9_recon.c $M/vp9_simd.c $M/vp9_lf.c $M/vp9_tables.c -lm || exit 1
ASAN_OPTIONS=detect_leaks=0 ./build-host/t_meng tests/media/vec
