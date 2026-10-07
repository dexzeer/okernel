#!/bin/sh
# Build the freestanding QuickJS test runner (tests/qjs/fsrun.c) from the same
# objects/flags the kernel uses, then run QuickJS's test suite with it:
#   sh tests/qjs/build.sh [test.js ...]
set -e
cd "$(dirname "$0")/../.."
OUT=build-host/qjs-fs
mkdir -p $OUT
GI=$(gcc -print-file-name=include)
F="-m32 -nostdinc -Isrc/qjs/libc -Isrc/qjs/libm -Isrc/qjs -isystem $GI -fno-pic -fno-pie -O2 \
   -fno-strict-aliasing -fwrapv -fno-stack-protector -mno-sse -mno-sse2 -mno-mmx -mfpmath=387 \
   -fno-asynchronous-unwind-tables -D_GNU_SOURCE -DQJS_NO_ATOMICS -DCONFIG_VERSION=\"2026-06-04\" -w"
OBJS=""
for f in src/qjs/quickjs.c src/qjs/cutils.c src/qjs/libregexp.c src/qjs/libunicode.c src/qjs/dtoa.c \
         src/qjs/qjs_libc.c src/qjs/libm/*.c src/string.c tests/qjs/fsrun.c; do
    o=$OUT/$(echo $f | tr / _ | sed 's/\.c$/.o/')
    X=""
    case $f in src/string.c) X="-ffreestanding -fno-builtin";; src/qjs/qjs_libc.c) X="-fno-tree-loop-distribute-patterns";; esac
    if [ ! -f $o ] || [ $f -nt $o ]; then gcc $F $X -c $f -o $o; fi
    OBJS="$OBJS $o"
done
ld -m elf_i386 -static -nostdlib -e _start -o build-host/qjs-fsrun $OBJS $(gcc -m32 -print-libgcc-file-name)
echo "built build-host/qjs-fsrun"
if [ $# -gt 0 ]; then ./build-host/qjs-fsrun "$@"; fi
