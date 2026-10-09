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
   src/web/wjs.c src/web/wjs_dom.c src/web/wjs_sys.c src/web/wcookie.c"
[ -f src/web/image.c ] && W="$W src/web/image.c"
nasm -f elf32 src/web/fontdata.asm -o build-host/fontdata.o
nasm -f elf32 src/web/wjs_prelude.asm -o build-host/wjs_prelude.o
# the ojs JavaScript engine (src/ojs) + our libm; objects are cached per
# sanitizer flavour and rebuilt when their sources or headers change
QD=build-host/ojs-web-nopie$(printf '%s' "$SAN$OPT" | tr -c 'a-zA-Z0-9' '_')
mkdir -p $QD
QOBJ=""
for f in gc str obj util conv num unicode unicode_data lex parse compile ops vm realm b_object b_function          b_error b_iter b_array b_string b_number b_global b_promise generator b_map b_proxy b_typed bigint          b_json b_date re b_regexp module api srctext; do
    if [ ! -f $QD/$f.o ] || [ -n "$(find src/ojs -newer $QD/$f.o -name '*.[ch]' | head -1)" ]; then
        X=""; [ $f = vm ] && X="--param max-goto-duplication-insns=64"   # interpreter dispatch
        gcc -m32 -fno-pie -fno-pic $OPT -g $SAN -fno-strict-aliasing -w $X -c src/ojs/$f.c -o $QD/$f.o
    fi
    QOBJ="$QOBJ $QD/$f.o"
done
if [ ! -f $QD/kmath.o ] || [ src/kmath.c -nt $QD/kmath.o ]; then
    gcc -m32 -fno-pie -fno-pic $OPT -g $SAN -DKMATH_NO_STD_NAMES -w -c src/kmath.c -o $QD/kmath.o
fi
QOBJ="$QOBJ $QD/kmath.o"
# non-PIE like the kernel (PIE costs a register on i386)
CF="-m32 -fno-pie -fno-pic -no-pie $OPT -g $SAN -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Isrc -Itests/web"
gcc $CF -o build-host/wrender tests/web/render.c $W build-host/fontdata.o build-host/wjs_prelude.o $QOBJ -lm
echo built build-host/wrender
gcc $CF -o build-host/wbrowse tests/web/wbrowse.c $W build-host/fontdata.o build-host/wjs_prelude.o $QOBJ -lm
echo built build-host/wbrowse
