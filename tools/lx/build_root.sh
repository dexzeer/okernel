#!/bin/sh
# The root file system of the Linux/Windows subsystem that ships with the desktop image: own code only (no host libraries,
# no glibc programs; those are test inputs, see build_tests.sh).
#   sh tools/lx/build_root.sh [dir]        -> dir (default build-host/sysroot) and dir.tar next to it
# Contents: /bin = the programs of lx/src and the X server xokai (+ its test client), /windows/system32 = our kernel32,
# msvcrt, user32, gdi32 and /win = the Windows demo programs (lx/win/tests).
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-build-host/sysroot}
rm -rf "$OUT"; mkdir -p "$OUT/bin" "$OUT/tmp"
CC="gcc -static -nostdlib -ffreestanding -fno-stack-protector -fno-pic -no-pie -O2 -Wall"
for f in lx/src/*.c; do
    $CC -o "$OUT/bin/$(basename $f .c)" $f
done
$CC -Wno-unused-function -o "$OUT/bin/xokai" lx/xsrv/*.c
for f in lx/xsrv/clients/*.c; do
    $CC -o "$OUT/bin/$(basename $f .c)" $f
done
sh lx/win/build.sh > /dev/null
cp -a build-host/winroot/. "$OUT/"
find "$OUT" -name "*.elf" -delete           # the linked ELF next to each PE (pebuild keeps it for nm)
tar --format=ustar -cf "$OUT.tar" -C "$OUT" .
echo built "$OUT.tar"
