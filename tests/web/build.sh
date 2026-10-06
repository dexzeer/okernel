#!/bin/sh
# Build the host web-engine tools (run from the repo root inside WSL/Linux).
set -e
mkdir -p build-host tests/web/out
SAN=${SAN:-}
OPT=${OPT:--O2}
W="src/web/wdom.c src/web/html5.c src/web/charset.c src/web/css_values.c src/web/css_parse.c \
   src/web/css_select.c src/web/css_style.c src/web/wurl.c src/web/font.c src/web/raster.c \
   src/web/lay_tree.c src/web/lay_main.c src/web/lay_inline.c src/web/lay_flex.c src/web/lay_grid.c \
   src/web/lay_table.c src/web/lay_dl.c src/web/paint.c src/web/svg.c src/web/wdoc.c src/cjk.c"
[ -f src/web/image.c ] && W="$W src/web/image.c"
nasm -f elf32 src/web/fontdata.asm -o build-host/fontdata.o
gcc -m32 $OPT -g $SAN -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Isrc -Itests/web \
    -o build-host/wrender tests/web/render.c $W build-host/fontdata.o
echo built build-host/wrender
