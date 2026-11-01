#!/bin/sh
# Build okai for Windows (32-bit) with MinGW-w64.
#
#   sudo apt install gcc-mingw-w64-i686     (once, in WSL)
#   sh win32/build.sh                       (or: make win)
#
# Output: build-host/okai.exe. Incremental: objects land in build-win32/ and a
# changed header rebuilds everything (the dependency list is coarse on purpose:
# a stale struct size is the bug class AGENTS.md warns about).
set -e
cd "$(dirname "$0")/.."

CC=${CC:-i686-w64-mingw32-gcc}
NASM=${NASM:-nasm}
OUT=${OUT:-build-host/okai.exe}
OBJ=build-win32
mkdir -p "$OBJ" build-host

command -v "$CC" >/dev/null 2>&1 || {
    # no-root install path (see .scratch/mingw-setup.sh) before giving up
    if [ -x "$HOME/mingw/root/usr/bin/i686-w64-mingw32-gcc-posix" ]; then
        CC="$HOME/mingw/root/usr/bin/i686-w64-mingw32-gcc-posix"
    else
        echo "error: $CC not found. In WSL: sudo apt install gcc-mingw-w64-i686" >&2
        echo "       (or unpack the .deb files under \$HOME/mingw/root, see win32/README.md)" >&2
        exit 1
    fi
}

CF="-O2 -g -msse2 -fno-strict-aliasing -fno-stack-protector -fno-pic -fno-pie \
    -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
    -Wno-missing-field-initializers -Wno-unused-function -Wno-sign-compare \
    -Isrc -Iwin32 -Iwin32/shim -D_WIN32_WINNT=0x0601 -DWIN32_LEAN_AND_MEAN \
    -DOKAI_PORT -MMD -MP"

WEB="src/web/wdom.c src/web/html5.c src/web/charset.c src/web/css_values.c \
     src/web/css_parse.c src/web/css_select.c src/web/css_style.c src/web/wurl.c \
     src/web/font.c src/web/raster.c src/web/lay_tree.c src/web/lay_main.c \
     src/web/lay_inline.c src/web/lay_flex.c src/web/lay_grid.c src/web/lay_table.c \
     src/web/lay_dl.c src/web/paint.c src/web/svg.c src/web/wdoc.c src/web/image.c \
     src/web/pscan.c src/web/wjs.c src/web/wjs_dom.c src/web/wjs_media.c \
     src/web/wjs_canvas.c src/web/wjs_crypto.c src/web/wjs_wasm.c src/web/wasm.c \
     src/web/wcanvas.c src/web/wjs_sys.c src/web/wcookie.c src/cjk.c src/web/bidi.c src/web/cstyles.c src/ablock.c"

MEDIA="src/media/meng.c src/media/webm.c src/media/opus_dec.c src/media/opus_packet.c \
       src/media/celt.c src/media/celt_tables.c src/media/ec.c src/media/vp9.c \
       src/media/vp9_recon.c src/media/vp9_simd.c src/media/vcvt.c src/media/vp9_lf.c \
       src/media/vp9_tables.c src/media/mp4.c src/media/aac.c src/media/aac_tables.c \
       src/media/resample.c src/media/h264.c src/media/h264_slice.c src/media/h264_recon.c \
       src/media/h264_cabac.c src/media/h264_inter.c src/media/h264_lf.c src/media/h264_tables.c"

CRYPTO="src/crypto/sha1.c src/crypto/sha256.c src/crypto/sha512.c src/crypto/aead.c \
        src/crypto/aes.c src/crypto/certverify.c src/crypto/chacha20.c src/crypto/der.c \
        src/crypto/ec.c src/crypto/hkdf.c src/crypto/hmac.c src/crypto/ocsp.c \
        src/crypto/poly1305.c src/crypto/rand.c src/crypto/roots.c src/crypto/rsa.c \
        src/crypto/tls_client.c src/crypto/tls_handshake.c src/crypto/tls_keysched.c \
        src/crypto/tls_record.c src/crypto/x25519.c src/crypto/x509.c"

NET="src/net/fetch.c src/net/fcache.c src/net/h2.c src/net/hpack.c src/net/hpack_tables.c"

OJS="src/ojs/gc.c src/ojs/str.c src/ojs/obj.c src/ojs/util.c src/ojs/conv.c src/ojs/num.c \
     src/ojs/unicode.c src/ojs/unicode_data.c src/ojs/lex.c src/ojs/parse.c src/ojs/compile.c \
     src/ojs/ops.c src/ojs/vm.c src/ojs/realm.c src/ojs/b_object.c src/ojs/b_function.c \
     src/ojs/b_error.c src/ojs/b_iter.c src/ojs/b_array.c src/ojs/b_string.c src/ojs/b_number.c \
     src/ojs/b_global.c src/ojs/b_promise.c src/ojs/generator.c src/ojs/b_map.c src/ojs/b_proxy.c \
     src/ojs/b_typed.c src/ojs/bigint.c src/ojs/b_json.c src/ojs/b_date.c src/ojs/re.c \
     src/ojs/b_regexp.c src/ojs/module.c src/ojs/api.c src/ojs/srctext.c"

SHELL="src/okai.c src/okai_ui.c"
PLAT="win32/win_main.c win32/win_gfx.c win32/win_window.c win32/win_sys.c win32/win_net.c \
      win32/win_audio.c win32/win_heap.c"

# A header change rebuilds everything: okai is a single binary and a stale
# struct size across objects is exactly the bug class -MMD -MP exists for.
if [ ! -f "$OBJ/.hdrs" ]; then : > "$OBJ/.hdrs"; fi
if [ -n "$(find src win32 -name '*.[ch]' -newer "$OBJ/.hdrs" 2>/dev/null | head -1)" ]; then
    touch "$OBJ/.hdrs"
fi

objs=""
n=0
for src in $WEB $MEDIA $CRYPTO $NET $OJS $SHELL $PLAT; do
    o="$OBJ/$(printf '%s' "$src" | tr '/\\' '__' | sed 's/\.c$/.o/')"
    extra=""
    case "$src" in
        src/ojs/vm.c) extra="--param max-goto-duplication-insns=64 -w" ;;
        src/ojs/*)    extra="-w" ;;
    esac
    if [ ! -f "$o" ] || [ "$src" -nt "$o" ] || [ "$OBJ/.hdrs" -nt "$o" ]; then
        $CC $CF $extra -c "$src" -o "$o"
    fi
    objs="$objs $o"
    n=$((n + 1))
done

# ojs' libm (same build as the kernel: no std names)
o="$OBJ/src_kmath.o"
if [ ! -f "$o" ] || [ src/kmath.c -nt "$o" ] || [ "$OBJ/.hdrs" -nt "$o" ]; then
    $CC $CF -DKMATH_NO_STD_NAMES -w -c src/kmath.c -o "$o"
fi
objs="$objs $o"

# Embedded blobs, assembled as COFF objects (--prefix _ is the i386 C name
# mangling: wfd_sans_r -> _wfd_sans_r).
for a in fontdata wjs_prelude callstack; do
    o="$OBJ/$a.obj"
    if [ ! -f "$o" ] || [ "src/web/$a.asm" -nt "$o" ] || [ src/web/wjs_prelude.js -nt "$o" ]; then
        $NASM -f win32 --prefix _ -I. -o "$o" "src/web/$a.asm"
    fi
    objs="$objs $o"
done

$CC -mwindows -o "$OUT" $objs \
    -lws2_32 -lwinmm -lgdi32 -luser32 -lole32 -luuid \
    -Wl,--stack,8388608 -static -static-libgcc

# Release binaries ship stripped; the unstripped copy of the SAME link is kept
# so a crash log's addresses can be resolved against it (nasm -g info, DWARF
# and the COFF symbol table all live there). STRIP=0 skips this.
if [ "${STRIP:-1}" != 0 ]; then
    STRIPBIN="${CC%gcc*}strip"
    if [ ! -x "$STRIPBIN" ] && command -v i686-w64-mingw32-strip >/dev/null 2>&1; then
        STRIPBIN=i686-w64-mingw32-strip
    fi
    DEBUG_OUT="${OUT%.exe}-debug.exe"
    cp "$OUT" "$DEBUG_OUT"
    if [ -x "$STRIPBIN" ]; then
        "$STRIPBIN" "$OUT"
    else
        echo "warning: no strip binary found; $OUT keeps its symbols" >&2
    fi
    echo "release: $OUT ($(du -h "$OUT" | cut -f1)), symbols: $DEBUG_OUT ($(du -h "$DEBUG_OUT" | cut -f1))"
fi

echo "built $OUT ($n sources)"
