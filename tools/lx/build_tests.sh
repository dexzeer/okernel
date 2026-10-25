#!/bin/sh
# Build the Linux-subsystem test root and a test ISO that boots the desktop kernel with it as a module.
#   sh tools/lx/build_tests.sh [extra-root-dir]     -> build-host/lxtest.iso
# The root holds: the programs of lx/src (own code, no C library), the glibc test programs of lx/test (built with the host
# gcc, static; test inputs only, they are not part of the system), and with HOSTBINS=1 the host's bash/ls/cat with their
# shared libraries (to try real dynamically linked programs).
set -e
cd "$(dirname "$0")/../.."
OUT=build-host/lxroot
rm -rf $OUT; mkdir -p $OUT/bin
for f in lx/src/*.c; do
    n=$(basename $f .c)
    gcc -static -nostdlib -ffreestanding -fno-stack-protector -fno-pic -no-pie -O2 -Wall -o $OUT/bin/$n $f
done
# the X11 server (several files, one binary) and its test clients
gcc -static -nostdlib -ffreestanding -fno-stack-protector -fno-pic -no-pie -O2 -Wall -Wno-unused-function -o $OUT/bin/xokai lx/xsrv/*.c
for f in lx/xsrv/clients/*.c; do
    gcc -static -nostdlib -ffreestanding -fno-stack-protector -fno-pic -no-pie -O2 -Wall -o $OUT/bin/$(basename $f .c) $f
done
for f in lx/test/gl_*.c; do
    n=$(basename $f .c | sed 's/^gl_/gl/')
    gcc -static -O2 -pthread -w -o $OUT/bin/$n $f
done
if [ -n "$HOSTBINS" ]; then
    mkdir -p $OUT/lib64 $OUT/lib/x86_64-linux-gnu
    for b in bash ls cat echo true sh; do cp /bin/$b $OUT/bin/ 2>/dev/null || true; done
    for f in lx/test/x_*.c; do gcc -O2 -w -o $OUT/bin/xl_$(basename $f .c | sed "s/^x_//") $f /usr/lib/x86_64-linux-gnu/libX11.so.6; done
    for b in $OUT/bin/bash $OUT/bin/ls $OUT/bin/cat $OUT/bin/xl_*; do
        for l in $(ldd $b 2>/dev/null | grep -o "/[^ ]*\.so[^ ]*"); do
            mkdir -p $OUT$(dirname $l); cp -L $l $OUT$l
        done
    done
fi
# the Windows side (kernel32/msvcrt/... and test programs), built by lx/win/build.sh
if [ -f lx/win/build.sh ]; then sh lx/win/build.sh > /dev/null; cp -a build-host/winroot/. $OUT/; fi
if [ -n "$1" ]; then cp -a "$1"/. $OUT/; fi
tar --format=ustar -cf build-host/lxtest.tar -C $OUT .
rm -rf build-host/lxiso; mkdir -p build-host/lxiso/boot/grub
cp kanarchy-desktop.bin build-host/lxiso/boot/kanarchy.bin
cp build-host/lxtest.tar build-host/lxiso/boot/lxroot.tar
cat > build-host/lxiso/boot/grub/grub.cfg <<EOF
set timeout=0
set default=0
menuentry "KAnarchy OS" {
    set gfxpayload=1920x1080x32
    multiboot /boot/kanarchy.bin
    module /boot/lxroot.tar
    boot
}
EOF
grub-mkrescue -o build-host/lxtest.iso build-host/lxiso 2>/dev/null
echo built build-host/lxtest.iso
