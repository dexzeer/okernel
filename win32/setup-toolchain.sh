#!/bin/sh
# Install the 32-bit MinGW-w64 toolchain for the okai Windows port WITHOUT
# root (no `sudo apt install`): unpack the .deb packages into ~/mingw/root.
# build.sh picks that location up automatically when i686-w64-mingw32-gcc is
# not on PATH.
#
#   sh win32/setup-toolchain.sh
#
# With root, the short way is:  sudo apt install gcc-mingw-w64-i686
set -e
DEST="$HOME/mingw"
mkdir -p "$DEST/debs"
cd "$DEST/debs"

# gcc-mingw-w64-i686-posix (not -win32): it carries clock_gettime and the
# pthreads model; okai does not need them at run time but the engine headers
# use <time.h> features that the win32 variant does not declare.
PKGS="binutils-mingw-w64-i686 mingw-w64-i686-dev mingw-w64-common \
      gcc-mingw-w64-i686-posix gcc-mingw-w64-base gcc-mingw-w64-i686-posix-runtime"

apt-get download $PKGS
cd "$DEST"
mkdir -p root
for d in debs/*.deb; do dpkg -x "$d" root; done

# The packages only create the alternatives links in their postinst scripts,
# which dpkg -x does not run.
B=root/usr/bin
ln -sf i686-w64-mingw32-gcc-posix "$B/i686-w64-mingw32-gcc"
ln -sf i686-w64-mingw32-g++-posix "$B/i686-w64-mingw32-g++"

export PATH="$DEST/$B:$PATH"
i686-w64-mingw32-gcc --version | head -1
printf 'int main(void){return 0;}\n' > probe.c
i686-w64-mingw32-gcc -O2 probe.c -o probe.exe
echo "toolchain ready in $DEST/root (build.sh finds it automatically)"
