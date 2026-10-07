#!/bin/sh
# Build the host web-engine tools (run from the repo root inside WSL/Linux):
#   build-host/wrender  - corpus page -> PPM (no network, no scripts)
#   build-host/wbrowse  - live page + scripts (curl, cached) -> PPM + console
# ASan: SAN="-fsanitize=address,undefined" OPT=-O1 sh tests/web/build.sh
set -e
mkdir -p build-host tests/web/out
SAN=${SAN:-}
OPT=${OPT:--O2}
W="src/web/wdom.c src/web/html5.c src/web/charset.c src/web/css_values.c src/web/css_parse.c \
   src/web/css_select.c src/web/css_style.c src/web/wurl.c src/web/font.c src/web/raster.c \
   src/web/lay_tree.c src/web/lay_main.c src/web/lay_inline.c src/web/lay_flex.c src/web/lay_grid.c \
   src/web/lay_table.c src/web/lay_dl.c src/web/paint.c src/web/svg.c src/web/wdoc.c src/cjk.c \
   src/web/wjs.c src/web/wjs_dom.c src/web/wcookie.c"
[ -f src/web/image.c ] && W="$W src/web/image.c"
nasm -f elf32 src/web/fontdata.asm -o build-host/fontdata.o
nasm -f elf32 src/web/wjs_prelude.asm -o build-host/wjs_prelude.o
# QuickJS against the host libc (the kernel build uses src/qjs/libc instead);
# objects are cached per sanitizer flavour.
QD=build-host/qjs-host$(printf '%s' "$SAN$OPT" | tr -c 'a-zA-Z0-9' '_')
mkdir -p $QD
QOBJ=""
for f in quickjs cutils libregexp libunicode dtoa; do
    if [ ! -f $QD/$f.o ] || [ src/qjs/$f.c -nt $QD/$f.o ]; then
        gcc -m32 $OPT -g $SAN -fwrapv -D_GNU_SOURCE -DQJS_NO_ATOMICS -DCONFIG_VERSION=\"2026-06-04\" -w \
            -c src/qjs/$f.c -o $QD/$f.o
    fi
    QOBJ="$QOBJ $QD/$f.o"
done
CF="-m32 $OPT -g $SAN -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Isrc -Isrc/qjs -Itests/web"
gcc $CF -o build-host/wrender tests/web/render.c $W build-host/fontdata.o build-host/wjs_prelude.o $QOBJ -lm
echo built build-host/wrender
gcc $CF -o build-host/wbrowse tests/web/wbrowse.c $W build-host/fontdata.o build-host/wjs_prelude.o $QOBJ -lm
echo built build-host/wbrowse
