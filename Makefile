# okernel Makefile

CC = gcc
AS = nasm
LD = ld

# Compiler flags
# -O2: without it the whole kernel built at -O0 — pixel loops (blit, strip
# repair, flush) ran 3-5x slower than needed and drags crawled.
# -fno-strict-aliasing: the network stack type-puns packet buffers through
# struct pointers (ip_header*, tcp on byte arrays) — UB under strict aliasing.
CFLAGS = -m32 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
         -nostartfiles -nodefaultlibs -fno-pic -fno-pie -mno-red-zone \
         -O2 -fno-strict-aliasing -MMD -MP \
         -Wall -Wextra -DKERNEL -Isrc

# Assembler flags
ASFLAGS = -f elf32

# Linker flags (text = low link; desktop = high-half link at 0xC0100000)
LDFLAGS = -m elf_i386 -T linker.ld
LDFLAGS_HIGH = -m elf_i386 -T linker-high.ld

# Common objects (no kernel.c, no desktop.c). ORDER: start.o FIRST — its
# .text must land at 0xC0100030 (right after .multiboot) so _start's LOW
# alias (phys 0x100030+0x30) is where GRUB jumps. isr.o second.
ASM_OBJ = boot/start.o boot/isr.o boot/smp_stubs.o boot/ap_tramp.o
COMMON_OBJ = src/gdt.o src/idt.o src/memory.o src/serial.o src/keyboard.o src/mouse.o src/string.o src/syscall.o

# Text mode objects
TEXT_OBJ = src/vga.o src/shell.o src/terminal.o src/hwinfo.o src/i915.o src/pcix.o src/acpi_pm.o src/text_stubs.o

# Text boot keeps VGA text mode: start.asm built WITHOUT the multiboot
# video request (desktop start.o asks GRUB for 1920x1080x32, which would
# leave the text kernel writing an invisible 0xB8000).
TEXT_ASM_OBJ = boot/start_text.o boot/isr.o
boot/start_text.o: boot/start.asm
	$(AS) $(ASFLAGS) -DTEXTMODE $< -o $@

# Desktop mode objects
# NOTE: -DKERNEL is set in CFLAGS below so the shared crypto/TLS source can
# switch its stdio logging to serial_printf and its RNG to the kernel CPRNG.
# NOTE: src/userland_seed.o embeds userland/gen_*.h bindata — it must rebuild
# whenever any gen header changes (the %.o rule only tracks HEADERS, and a
# stale seed silently ships last week's ELFs — bisected 2026-09-08 when a
# fresh forktest never reached the ISO).
USERLAND_GEN = $(wildcard userland/gen_*.h)

# Web engine (src/web): HTML5 parser, CSS, layout, painter, fonts, images.
# Desktop-only. fontdata.o incbins the TrueType faces in fonts/; callstack.o
# is the trampoline okai uses to run the engine on its own 2MB stack.
WEB_OBJ = src/web/wdom.o src/web/html5.o src/web/charset.o src/web/wurl.o \
          src/web/css_values.o src/web/css_parse.o src/web/css_select.o src/web/css_style.o \
          src/web/lay_tree.o src/web/lay_main.o src/web/lay_inline.o src/web/lay_flex.o \
          src/web/lay_grid.o src/web/lay_table.o src/web/lay_dl.o \
          src/web/paint.o src/web/raster.o src/web/font.o src/web/svg.o src/web/image.o \
          src/web/wdoc.o src/web/pscan.o src/web/fontdata.o src/web/callstack.o \
          src/web/wjs.o src/web/wjs_dom.o src/web/wjs_media.o src/web/wjs_canvas.o src/web/wjs_crypto.o src/web/wjs_wasm.o src/web/wasm.o src/web/wcanvas.o src/web/wjs_sys.o src/web/wjs_prelude.o src/web/wcookie.o
WEB_HEADERS := $(wildcard src/web/*.h) $(wildcard src/ojs/*.h)
src/wallpaper.o: src/wallpaper.asm tools/wallpaper/wallpaper.jpg
	$(AS) $(ASFLAGS) $< -o $@
src/rtw/rtwfw.o: src/rtw/rtwfw.asm firmware/rtw8821c_fw.bin
	$(AS) $(ASFLAGS) $< -o $@
src/web/fontdata.o: src/web/fontdata.asm $(wildcard fonts/*.ttf)
	$(AS) $(ASFLAGS) $< -o $@
src/web/wjs_prelude.o: src/web/wjs_prelude.asm src/web/wjs_prelude.js
	$(AS) $(ASFLAGS) $< -o $@

# ojs (src/ojs): our JavaScript engine for page scripts (see src/ojs/README).
# x87 doubles only (-mno-sse: the kernel FNSAVEs per thread and each realm runs
# at 53-bit precision); our libm (src/kmath.c), our 64-bit division helpers
# (src/divdi3.c - no libgcc), the realm clock (src/kclock.c). vm.c is the
# interpreter: GCC copies its dispatch jump into every instruction.
OJS_CFLAGS = -m32 -ffreestanding -fno-pic -fno-pie -mno-red-zone -O2 -fno-strict-aliasing \
             -fno-stack-protector -mno-sse -mno-sse2 -mno-mmx -mfpmath=387 \
             -fno-asynchronous-unwind-tables -DKERNEL -Isrc -MMD -MP \
             -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
OJS_SRC = gc str obj util conv num unicode unicode_data lex parse compile ops vm realm b_object b_function \
          b_error b_iter b_array b_string b_number b_global b_promise generator b_map b_proxy b_typed bigint \
          b_json b_date re b_regexp module api srctext
OJS_OBJ = $(patsubst %,src/ojs/%.o,$(OJS_SRC)) src/kmath.o src/kclock.o src/divdi3.o
src/ojs/vm.o: src/ojs/vm.c
	$(CC) $(OJS_CFLAGS) --param max-goto-duplication-insns=64 -c $< -o $@
src/ojs/%.o: src/ojs/%.c
	$(CC) $(OJS_CFLAGS) -c $< -o $@
# kmath: computes in x87 extended precision (see the file header)
src/kmath.o: src/kmath.c
	$(CC) $(OJS_CFLAGS) -c $< -o $@
src/divdi3.o: src/divdi3.c
	$(CC) $(OJS_CFLAGS) -c $< -o $@
# Media decoders (src/media): doubles/floats on the x87 like ojs, libm from src/kmath.c.
MEDIA_SRC = ec opus_packet celt celt_tables opus_dec webm vp9 vp9_recon vp9_simd vcvt vp9_lf vp9_tables mp4 aac aac_tables resample h264 h264_slice h264_recon h264_cabac h264_inter h264_lf h264_tables meng
MEDIA_OBJ = $(patsubst %,src/media/%.o,$(MEDIA_SRC))
src/media/%.o: src/media/%.c
	$(CC) $(OJS_CFLAGS) -c $< -o $@
src/userland_seed.o: $(USERLAND_GEN)
# The dmccl language (src/dmccl) and the game engine on top of it (src/game, src/gamewin.c): doubles on the x87 like ojs.
GAME_SRC = dmccl/dm_lex dmccl/dm_compile dmccl/dm_vm dmccl/dm_lib game/phys game/game game/game_chars game/game_gfx
GAME_OBJ = $(patsubst %,src/%.o,$(GAME_SRC)) src/gamewin.o src/game/levels.o
src/dmccl/%.o: src/dmccl/%.c
	$(CC) $(OJS_CFLAGS) -c $< -o $@
src/game/%.o: src/game/%.c
	$(CC) $(OJS_CFLAGS) -c $< -o $@
src/gamewin.o: src/gamewin.c
	$(CC) $(OJS_CFLAGS) -c $< -o $@
src/game/levels.o: src/game/levels.asm games/level1.dmccl games/level2.dmccl games/level3.dmccl games/level4.dmccl
	$(AS) $(ASFLAGS) $< -o $@
DESKTOP_OBJ = src/mplay.o $(MEDIA_OBJ) $(GAME_OBJ) src/hda.o src/ac97.o src/audio_rate.o src/pcix.o src/acpi_pm.o src/xhci.o src/wifigui.o src/rtw/rtw_hw.o src/rtw/rtw_pci.o src/rtw/rtw_wifi.o src/rtw/wsta.o src/rtw/wcrypto.o src/rtw/rtw_tables.o src/rtw/rtwfw.o src/hid.o src/devlog.o src/dwi2c.o src/hidi2c.o src/ptp.o src/i2caml.o src/i2chid.o src/klog.o src/virtio.o src/vgpu.o src/i915.o src/cpufreq.o src/fpu.o src/simd.o src/simd_test.o src/prof.o src/acpi.o src/lapic.o src/smp.o src/graphics.o src/window.o src/paging.o src/process.o src/sched.o src/sys_proc.o src/elf.o src/spinlock.o src/ata.o src/pfs.o src/userland_seed.o src/net/pci.o src/net/e1000.o src/net/network.o src/filesystem.o src/editor.o src/okai.o src/okai_ui.o src/taskbar.o src/cursor.o src/wallpaper.o src/textslot.o src/cjk.o \
              src/font_data.o $(WEB_OBJ) $(OJS_OBJ) \
             src/crypto/sha256.o src/crypto/sha1.o src/crypto/ocsp.o src/crypto/hmac.o src/crypto/hkdf.o \
             src/crypto/aead.o src/crypto/aes.o src/crypto/chacha20.o src/crypto/poly1305.o \
             src/crypto/x25519.o src/crypto/tls_record.o src/crypto/tls_handshake.o \
             src/crypto/tls_keysched.o src/crypto/tls_client.o src/crypto/rand.o \
             src/crypto/sha512.o src/crypto/der.o src/crypto/x509.o \
             src/crypto/rsa.o src/crypto/ec.o src/crypto/roots.o \
             src/crypto/certverify.o src/rtc.o \
             src/net/fcache.o src/net/hpack.o src/net/hpack_tables.o src/net/h2.o src/net/fetch.o

.PHONY: laptop all text desktop clean run debug host-tests host-tests-asan media-tests diff-oracle diff-test tls-interop web-tests

# ---- Web engine host tests (src/web) ----
# Offline must-pass: CSS cascade unit tests, font engine. With the corpus
# fetched (python3 tools/fetch_corpus.py) also: HTML5 tree diff vs html5lib,
# image decoders vs PIL, every corpus page through the renderer (see
# tests/web/; ASan: SAN="-fsanitize=address,undefined" sh tests/web/build.sh).
WEB_HOST_SRC = src/web/wdom.c src/web/html5.c src/web/charset.c src/web/css_values.c src/web/css_parse.c \
               src/web/css_select.c src/web/css_style.c src/web/wurl.c src/web/font.c src/web/raster.c \
               src/web/lay_tree.c src/web/lay_main.c src/web/lay_inline.c src/web/lay_flex.c src/web/lay_grid.c \
               src/web/lay_table.c src/web/lay_dl.c src/web/paint.c src/web/svg.c src/web/wdoc.c src/web/image.c src/cjk.c src/web/wcookie.c
# media engine sources the script realm links (wjs_media.c)
WEB_HOST_MEDIA = src/web/wjs_media.c src/web/wjs_canvas.c src/web/wjs_crypto.c src/web/wjs_wasm.c src/web/wasm.c src/crypto/sha1.c src/crypto/sha256.c src/crypto/sha512.c src/web/wcanvas.c src/media/meng.c src/media/webm.c src/media/opus_dec.c src/media/opus_packet.c src/media/celt.c src/media/celt_tables.c src/media/ec.c src/media/vp9.c src/media/vp9_recon.c src/media/vp9_simd.c src/media/vcvt.c src/media/vp9_lf.c src/media/vp9_tables.c src/media/mp4.c src/media/aac.c src/media/aac_tables.c src/media/resample.c src/media/h264.c src/media/h264_slice.c src/media/h264_recon.c src/media/h264_cabac.c src/media/h264_inter.c src/media/h264_lf.c src/media/h264_tables.c
# the ojs objects tests/web/build.sh made (globbed when the recipe runs)
WEB_HOST_LINK = build-host/fontdata.o build-host/wjs_prelude.o build-host/ojs-web-nopie_O2/*.o -lm
WEB_HOST_CC = gcc -m32 -msse2 -fno-pie -fno-pic -no-pie -O2 -Isrc -Itests/web
web-tests:
	mkdir -p build-host
	sh tests/web/build.sh
	$(WEB_HOST_CC) -o build-host/t_webcss tests/web/test_css.c $(WEB_HOST_SRC) src/web/wjs.c src/web/wjs_dom.c src/web/wjs_sys.c $(WEB_HOST_MEDIA) $(WEB_HOST_LINK) && ./build-host/t_webcss | tail -n 2
	$(WEB_HOST_CC) -o build-host/t_font tests/web/test_font.c $(WEB_HOST_SRC) src/web/wjs.c src/web/wjs_dom.c src/web/wjs_sys.c $(WEB_HOST_MEDIA) $(WEB_HOST_LINK) && ./build-host/t_font | tail -n 2
	$(WEB_HOST_CC) -o build-host/t_lazy tests/web/test_lazy.c $(WEB_HOST_SRC) src/web/wjs.c src/web/wjs_dom.c src/web/wjs_sys.c $(WEB_HOST_MEDIA) $(WEB_HOST_LINK) && ./build-host/t_lazy | tail -n 2
	gcc -m32 -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o build-host/t_hpack tests/test_hpack.c && ASAN_OPTIONS=detect_leaks=1 ./build-host/t_hpack | tail -n 1
	gcc -m32 -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o build-host/t_h2 tests/test_h2.c && ASAN_OPTIONS=detect_leaks=1 ./build-host/t_h2 | tail -n 1
	$(WEB_HOST_CC) -fsanitize=address,undefined -fno-sanitize-recover=undefined -o build-host/t_pscan tests/web/test_pscan.c src/web/pscan.c src/web/wurl.c && ./build-host/t_pscan | tail -n 1
	./build-host/wbrowse tests/web/js/basic.html 800 600 build-host/js-basic.ppm 3 | grep RESULT
	./build-host/wbrowse tests/web/js/fragparse.html 640 400 build-host/js-frag.ppm 3 | grep 'RESULT ALLPASS'
	./build-host/wbrowse tests/web/js/tostr.html 640 400 build-host/js-tostr.ppm 3 | grep 'RESULT ALLPASS'
	./build-host/wbrowse tests/web/js/observers.html 800 600 build-host/js-obs.ppm 4 | grep 'RESULT ALLPASS'
	./build-host/wbrowse tests/web/js/ce_import.html 800 600 build-host/js-ce.ppm 3 | grep 'RESULT ALLPASS'
	./build-host/wbrowse tests/web/js/cssvars.html 400 300 build-host/js-vars.ppm 3 | grep 'RESULT ALLPASS'
	./build-host/wbrowse tests/web/js/mocd.html 400 300 build-host/js-mocd.ppm 3 | grep 'RESULT ALLPASS'
	sh tests/web/test_media.sh
	sh tests/web/test_webapi.sh
	sh tests/web/test_wasm.sh
	sh tests/web/test_canvas.sh
	gcc -m32 -O2 -Isrc -Itests/web -o build-host/t_idct tests/web/test_idct.c -lm && ./build-host/t_idct | tail -n 1
	gcc -m32 -O2 -o build-host/t_divdi3 tests/test_divdi3.c && ./build-host/t_divdi3 2000000
	sh tests/ojs/build.sh && ./build-host/ojs_run tests/ojs/smoke.js | tail -n 1 | grep 'ALL OK'
	OJS_MEMLIMIT=64 ./build-host/ojs_run tests/ojs/memlimit.js | grep 'ALL OK'
	./build-host/ojs_run tests/ojs/manyconsts.js | grep 'ALL OK'
	./build-host/ojs_run tests/ojs/ic_semantics.js | grep 'ALL OK'
	OJS_GCSTRESS=3 ./build-host/ojs_run tests/ojs/ic_semantics.js | grep 'ALL OK'

# ---- Host tests (no QEMU; run from repo root — suites load tests/fixtures/*) ----
# Offline must-pass: tls_crypto, css, subres, pki, adversarial (fast -O2 build;
# the ASan build is 2-3x slower — see the commented line in the recipe).
# Informational (never gates): text_decode (charset/entity/slot coverage)
# and the live-network tls_client_test (needs internet to example.com:443).
HOST_CRYPTO_SRC = src/crypto/tls_client.c src/crypto/tls_record.c src/crypto/tls_handshake.c src/crypto/aes.c \
              src/crypto/tls_keysched.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/ocsp.c src/crypto/sha512.c \
              src/crypto/hmac.c src/crypto/hkdf.c src/crypto/aead.c \
              src/crypto/chacha20.c src/crypto/poly1305.c src/crypto/x25519.c \
              src/crypto/der.c src/crypto/x509.c src/crypto/rsa.c \
              src/crypto/ec.c src/crypto/certverify.c src/crypto/roots.c
host-tests:
	mkdir -p build-host
	gcc -m32 -O2 -Isrc/crypto -Isrc -o build-host/t_tls_crypto tests/test_tls_crypto.c src/crypto/sha256.c src/crypto/sha1.c src/crypto/sha512.c src/crypto/hmac.c src/crypto/hkdf.c src/crypto/aead.c src/crypto/chacha20.c src/crypto/poly1305.c src/crypto/x25519.c && ./build-host/t_tls_crypto | tail -n 2
	gcc -m32 -O2 -Isrc -Isrc/crypto -o build-host/t_chachapoly tests/test_chachapoly.c src/crypto/chacha20.c src/crypto/poly1305.c && ./build-host/t_chachapoly | tail -n 3
	gcc -m32 -O2 -mfpmath=387 -DKMATH_NO_STD_NAMES -o build-host/t_kmath tests/test_kmath.c src/kmath.c -lm && ./build-host/t_kmath 100000 | tail -n 1
	gcc -m32 -O2 -Isrc -Isrc/crypto -o build-host/t_rng tests/test_rng.c src/crypto/rand.c src/crypto/chacha20.c src/crypto/sha256.c && ./build-host/t_rng | tail -n 2
	gcc -m32 -O2 -Isrc/crypto -Isrc -o build-host/t_strict tests/test_crypto_strict.c $(HOST_CRYPTO_SRC) && ./build-host/t_strict | tail -n 2
	gcc -m32 -O2 -DKERNEL=0 -Isrc -o build-host/t_edtext tests/test_editor_text.c src/editor.c src/textslot.c && ./build-host/t_edtext | tail -n 2
	gcc -m32 -O1 -fsanitize=address,undefined -Isrc -o build-host/t_acpi tests/test_acpi.c && ./build-host/t_acpi | tail -n 1
	gcc -m32 -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Isrc -o build-host/t_hid tests/test_hid.c src/hid.c && ./build-host/t_hid | tail -n 1
	gcc -m32 -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Isrc -o build-host/t_touch tests/test_touch.c src/hid.c src/ptp.c src/dwi2c.c src/hidi2c.c src/i2caml.c && ./build-host/t_touch | tail -n 1
	gcc -m32 -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Isrc -o build-host/t_rtw -DRTWPCI_HOST tests/test_rtw.c src/rtw/rtw_pci.c src/rtw/rtw_tables.c src/rtw/wsta.c src/rtw/wcrypto.c src/crypto/sha1.c src/crypto/aes.c && ./build-host/t_rtw | tail -n 1
	gcc -m32 -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o build-host/t_wcrypto tests/test_wcrypto.c src/rtw/wcrypto.c src/crypto/sha1.c src/crypto/aes.c && (python3 tests/wcrypto_vectors.py 1 | ./build-host/t_wcrypto | tail -n 1)
	gcc -m32 -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o build-host/t_wsta tests/test_wsta.c src/rtw/wsta.c src/rtw/wcrypto.c src/crypto/sha1.c src/crypto/aes.c && ./build-host/t_wsta | tail -n 1
	gcc -m32 -O1 -fsanitize=address,undefined -o build-host/t_i915 tests/test_i915.c src/i915.c && ./build-host/t_i915 | tail -n 1
	$(MAKE) web-tests
	gcc -m32 -O2 -Isrc/crypto -o build-host/t_aes tests/test_aes.c src/crypto/aes.c && (python3 tests/aes_vectors.py 300 2>/dev/null | ./build-host/t_aes - || ./build-host/t_aes) | tail -n 2
	gcc -m32 -O2 -Isrc/crypto -Isrc -o build-host/t_pki tests/test_pki.c $(HOST_CRYPTO_SRC) && ./build-host/t_pki | tail -n 2
	gcc -m32 -O2 -Isrc/crypto -Isrc -o build-host/t_alpn tests/test_alpn.c $(HOST_CRYPTO_SRC) && ./build-host/t_alpn | tail -n 1
	printf '#include <string.h>\n#include <stdint.h>\n#include "tls_client.h"\n' > build-host/dechunk_fn.c
	awk '/^int http_dechunk\(char\* buf, int len\) \{/{p=1} p{print} p&&/^}/{exit}' src/net/network.c >> build-host/dechunk_fn.c
	gcc -m32 -O1 -fsanitize=address,undefined -Isrc/crypto -Isrc -o build-host/t_dechunk tests/test_dechunk.c build-host/dechunk_fn.c $(HOST_CRYPTO_SRC) && ./build-host/t_dechunk | tail -n 1
	gcc -m32 -O2 -Isrc/crypto -Isrc -o build-host/t_adv tests/test_adversarial.c $(HOST_CRYPTO_SRC) && timeout 300 ./build-host/t_adv | tail -n 3
	-gcc -m32 -O2 -Isrc/crypto -Isrc -o build-host/t_tls_live tests/test_tls_client.c $(HOST_CRYPTO_SRC) && timeout 60 ./build-host/t_tls_live | tail -n 3; echo "(tls_client_test: needs internet; SKIP if unreachable)"

# Media decoders (src/media) against the RFC 6716 reference decoder's output; the vectors
# are checked in (tests/media/vec), regenerate with tests/media/gen_opus_vectors.py.
# -mfpmath=387 matches what the kernel executes (x87, no SSE).
media-tests:
	mkdir -p build-host
	gcc -m32 -O1 -mfpmath=387 -fsanitize=address,undefined -o build-host/t_celt tests/media/test_celt.c src/media/celt.c src/media/celt_tables.c src/media/ec.c src/media/opus_packet.c -lm
	gcc -m32 -O1 -mfpmath=387 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o build-host/t_celt_fuzz tests/media/fuzz_celt.c src/media/celt.c src/media/celt_tables.c src/media/ec.c -lm
	gcc -m32 -O1 -mfpmath=387 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o build-host/t_opus_ms tests/media/test_opus_ms.c src/media/opus_dec.c src/media/opus_packet.c src/media/celt.c src/media/celt_tables.c src/media/ec.c -lm
	ASAN_OPTIONS=detect_leaks=0 ./build-host/t_opus_ms tests/media/vec | tail -n 1
	gcc -m32 -O1 -mfpmath=387 -fsanitize=address,undefined -o build-host/t_opus tests/media/test_opus.c src/media/opus_dec.c src/media/opus_packet.c src/media/celt.c src/media/celt_tables.c src/media/ec.c -lm
	@test -d tests/media/vec || { echo "no test vectors: run tests/media/gen_opus_vectors.py (needs ffmpeg + the RFC reference decoder)"; exit 1; }
	@set -e; for f in tests/media/vec/opus_*.bit; do n=$${f%.bit}; ASAN_OPTIONS=detect_leaks=0 ./build-host/t_opus $$n.bit $$n.ref | tail -n 1 | sed "s|^|$$n: |"; done
	ASAN_OPTIONS=detect_leaks=0 ./build-host/t_celt_fuzz 50000 1 tests/media/vec/opus_celt_fb_st.bit tests/media/vec/opus_celt_ld_st_12k.bit tests/media/vec/opus_celt_2_5ms.bit | tail -n 1
	gcc -m32 -O1 -fsanitize=address,undefined -o build-host/t_webm tests/media/test_webm.c src/media/webm.c -lm
	gcc -m32 -O1 -fsanitize=address,undefined -o build-host/t_webm_unit tests/media/test_webm_unit.c src/media/webm.c -lm
	gcc -m32 -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o build-host/t_webm_fuzz tests/media/fuzz_webm.c src/media/webm.c -lm
	@set -e; for f in tests/media/vec/webm_*.webm; do n=$${f%.webm}; ASAN_OPTIONS=detect_leaks=0 ./build-host/t_webm $$n.webm $$n.pkts | tail -n 1 | sed "s|^|$$n: |"; done
	gcc -m32 -O1 -fsanitize=address,undefined -o build-host/t_arate tests/test_arate.c src/audio_rate.c -lm
	ASAN_OPTIONS=detect_leaks=0 ./build-host/t_arate | tail -n 1
	gcc -m32 -O1 -msse2 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o build-host/t_vcvt tests/media/test_vcvt.c src/media/vcvt.c
	./build-host/t_vcvt | tail -n 1
	gcc -m32 -O2 -msse2 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o build-host/t_vp9_lf tests/media/test_vp9_lf.c src/media/vp9_simd.c
	./build-host/t_vp9_lf | tail -n 1
	gcc -m32 -O1 -msse2 -fsanitize=address,undefined -o build-host/t_vp9 tests/media/test_vp9.c src/media/vp9.c src/media/vp9_recon.c src/media/vp9_simd.c src/media/vcvt.c src/media/vp9_lf.c src/media/vp9_tables.c src/media/webm.c -lm
	@set -e; for f in tests/media/vec/vp9_*.webm; do n=$${f%.webm}; ASAN_OPTIONS=detect_leaks=0 ./build-host/t_vp9 $$n.webm $$n.yuv | tail -n 1 | sed "s|^|$$n: |"; done
	gcc -m32 -O1 -msse2 -pthread -DVP9_TEST_THREADS -fsanitize=address,undefined -o build-host/t_vp9_par tests/media/test_vp9.c src/media/vp9.c src/media/vp9_recon.c src/media/vp9_simd.c src/media/vcvt.c src/media/vp9_lf.c src/media/vp9_tables.c src/media/webm.c -lm
	@set -e; for f in tests/media/vec/vp9_*.webm; do n=$${f%.webm}; VP9_PAR=1 ASAN_OPTIONS=detect_leaks=0 ./build-host/t_vp9_par $$n.webm $$n.yuv | tail -n 1 | sed "s|^|$$n (tiles + loop filter on threads): |"; done
	gcc -m32 -O1 -msse2 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o build-host/t_vp9_fuzz tests/media/fuzz_vp9.c src/media/vp9.c src/media/vp9_recon.c src/media/vp9_simd.c src/media/vcvt.c src/media/vp9_lf.c src/media/vp9_tables.c src/media/webm.c -lm
	ASAN_OPTIONS=detect_leaks=0 ./build-host/t_vp9_fuzz 400 1 tests/media/vec/vp9_inter.webm tests/media/vec/vp9_intra.webm tests/media/vec/vp9_tiles.webm tests/media/vec/vp9_aq3.webm tests/media/vec/vp9_yuv422.webm tests/media/vec/vp9_yuv440.webm tests/media/vec/vp9_p10.webm tests/media/vec/vp9_odd.webm | tail -n 1
	sh tests/media/build_meng.sh | tail -n 1
	sh tests/media/build_aac.sh | tail -n 30 | grep -v PASS; true
	gcc -m32 -O1 -fsanitize=address,undefined -o build-host/t_mp4 tests/media/test_mp4.c src/media/mp4.c -lm
	@set -e; for f in tests/media/vec/mp4_*.mp4; do n=$${f%.mp4}; ASAN_OPTIONS=detect_leaks=0 ./build-host/t_mp4 $$n.mp4 $$n.pkts | tail -n 1; done
	gcc -m32 -O1 -fsanitize=address,undefined -o build-host/t_resample tests/media/test_resample.c src/media/resample.c -lm
	ASAN_OPTIONS=detect_leaks=0 ./build-host/t_resample | tail -n 1
	sh tests/media/build_h264.sh | grep -v "PASS"; true
	ASAN_OPTIONS=detect_leaks=0 ./build-host/t_webm_unit | tail -n 1
	ASAN_OPTIONS=detect_leaks=0 ./build-host/t_webm_fuzz 20000 1 tests/media/vec/webm_av.webm tests/media/vec/webm_live.webm tests/media/vec/webm_audio_only.webm | tail -n 1

# Sanitizer run of the adversarial suite (review P0 infra): every parser,
# flight, and mock path under ASan+UBSan. Slow (~10 min, RSA-heavy) — run
# after crypto/TLS changes, not on every edit.
host-tests-asan:
	mkdir -p build-host
	gcc -m32 -O1 -g -fsanitize=address,undefined -Isrc/crypto -Isrc -o build-host/t_adv_asan tests/test_adversarial.c $(HOST_CRYPTO_SRC) && timeout 590 ./build-host/t_adv_asan | tail -n 3

# Independent differential: okernel primitives vs Python `cryptography`
# (OpenSSL-backed — fully independent C code). X25519 (both directions +
# low-order rejection agreement), SHA-256, HMAC, HKDF, ChaCha20-Poly1305
# (enc/dec/tamper). Needs: python3-cryptography. Run after crypto changes.
DIFF_ORACLE_SRC = src/crypto/sha256.c src/crypto/hmac.c src/crypto/hkdf.c \
              src/crypto/aead.c src/crypto/chacha20.c src/crypto/poly1305.c \
              src/crypto/x25519.c
diff-oracle:
	mkdir -p build-host
	gcc -m32 -O2 -Isrc/crypto -o build-host/diff_oracle tests/differential/diff_oracle.c $(DIFF_ORACLE_SRC)
diff-test: diff-oracle
	python3 tests/differential/test_crypto_diff.py 200

# Interop driver (manual): builds only — the run needs a local TLS 1.3
# server on 127.0.0.1 (openssl s_server, python ssl, or TLS-Attacker).
# Usage: start server with an at_* chain, then
#   ./build-host/t_tlsa [port] [twice] [root.der] [TLSA_ROOT=..] [TLSA_SLEEP=n]
# (25s overall deadline per round; PASS on HTTP bytes; the trust hook holds
# one root per process, default tests/adversarial/at_root.der)
# TLS-Attacker recipe (canned workflows are TLS1.2-only and correctly
# rejected — use the custom trace + explicit sig algos):
#   J=~/tls-att/jdk-21.0.12.1+1-jre/bin/java
#   $J -jar ~/tls-att/apps/TLS-Server.jar -port 4443 -cert <chain-with-root> \
#     -key <leaf.key> -version TLS13 -cipher TLS_CHACHA20_POLY1305_SHA256 \
#     -named_group ECDH_X25519 -signature_hash_algo ECDSA_SHA256 \
#     -signature_algo_cert ECDSA_SHA256 \
#     -workflow_input tests/tlsattacker-server13.xml
# (RSA: -signature_hash_algo RSA_PSS_RSAE_SHA256; P-384: ECDSA_SHA384.
# Schema learned from -workflow_output dumps; JAXB errors name the exact
# expected elements: configuredMessages, SupportedVersions, Application.)
tls-interop:
	mkdir -p build-host
	gcc -m32 -O2 -Isrc/crypto -Isrc -o build-host/t_tlsa tests/test_tls_attacker.c $(HOST_CRYPTO_SRC)

all: text

# ---- Text mode build ----
text: kanarchy-text.iso

kanarchy-text.bin: $(TEXT_ASM_OBJ) $(COMMON_OBJ) $(TEXT_OBJ) src/kernel.o
	$(LD) $(LDFLAGS) -o $@ $^

kanarchy-text.iso: kanarchy-text.bin
	mkdir -p isodir/boot/grub
	cp kanarchy-text.bin isodir/boot/kanarchy.bin
	echo 'set timeout=0' > isodir/boot/grub/grub.cfg
	echo 'set default=0' >> isodir/boot/grub/grub.cfg
	echo '' >> isodir/boot/grub/grub.cfg
	echo 'menuentry "KAnarchy OS (text)" {' >> isodir/boot/grub/grub.cfg
	echo '    set gfxpayload=text' >> isodir/boot/grub/grub.cfg
	echo '    multiboot /boot/kanarchy.bin' >> isodir/boot/grub/grub.cfg
	echo '    boot' >> isodir/boot/grub/grub.cfg
	echo '}' >> isodir/boot/grub/grub.cfg
	grub-mkrescue -o $@ isodir 2>/dev/null

# ---- Desktop mode build ----
desktop: kanarchy-desktop.iso

kanarchy-desktop.bin: $(ASM_OBJ) $(COMMON_OBJ) $(DESKTOP_OBJ) src/desktop.o linker-high.ld
	$(LD) $(LDFLAGS_HIGH) -o $@ $(ASM_OBJ) $(COMMON_OBJ) $(DESKTOP_OBJ) src/desktop.o

kanarchy-desktop.iso: kanarchy-desktop.bin
	rm -rf isodir
	mkdir -p isodir/boot/grub
	cp kanarchy-desktop.bin isodir/boot/kanarchy.bin
	echo 'set timeout=0' > isodir/boot/grub/grub.cfg
	echo 'set default=0' >> isodir/boot/grub/grub.cfg
	echo '' >> isodir/boot/grub/grub.cfg
	echo 'menuentry "KAnarchy OS" {' >> isodir/boot/grub/grub.cfg
	echo '    set gfxpayload=1920x1080x32' >> isodir/boot/grub/grub.cfg
	echo '    multiboot /boot/kanarchy.bin' >> isodir/boot/grub/grub.cfg
	echo '    boot' >> isodir/boot/grub/grub.cfg
	echo '}' >> isodir/boot/grub/grub.cfg
	grub-mkrescue -o $@ isodir 2>/dev/null

# ---- Assemble ----
%.o: %.asm
	$(AS) $(ASFLAGS) $< -o $@

# ---- Compile ----
# Objects depend on all headers — FONT_SCALE/layout constants live in
# graphics.h/window.h and stale objects with mixed metrics garble rendering.
# On top of that every compile writes a -MMD dependency file (included at
# the bottom): a struct grown in src/crypto/tls_client.h once left
# tls_net.o at the old size, tls_state_init's memset ran past it and zeroed
# the fetch-timeout clock — every HTTPS fetch "timed out" instantly.
HEADERS := $(wildcard src/*.h) $(wildcard src/net/*.h) $(wildcard src/crypto/*.h)
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

src/web/%.o: src/web/%.c $(HEADERS) $(WEB_HEADERS)
	$(CC) $(CFLAGS) -Wno-unused-parameter -c $< -o $@

# SSE2 helpers: the only objects the compiler may use XMM registers in. They run
# in thread context only (interrupt handlers must not touch XMM; see src/fpu.h).
src/simd.o src/simd_test.o: src/%.o: src/%.c $(HEADERS)
	$(CC) $(CFLAGS) -msse2 -fno-tree-vectorize -c $< -o $@

src/okai.o: src/okai.c $(HEADERS) $(WEB_HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Run ----
run: text
	qemu-system-i386 -cdrom kanarchy-text.iso -boot d

run-desktop: desktop
	qemu-system-i386 -accel kvm -accel tcg -m 512 -cdrom kanarchy-desktop.iso -boot d -vga std -device e1000,netdev=net0 -netdev user,id=net0 -fullscreen

debug: text
	qemu-system-i386 -cdrom kanarchy-text.iso -boot d -serial stdio

ALL_C_OBJ = $(COMMON_OBJ) $(TEXT_OBJ) $(DESKTOP_OBJ) src/kernel.o src/desktop.o

clean:
	rm -f $(ASM_OBJ) boot/start_text.o $(ALL_C_OBJ) $(ALL_C_OBJ:.o=.d)
	rm -f kanarchy-text.bin kanarchy-text.iso kanarchy-desktop.bin kanarchy-desktop.iso
	rm -rf isodir

-include $(wildcard $(ALL_C_OBJ:.o=.d))

# ---- Test build: boot CPU forced into x2APIC mode, SMP self test at boot ----
# Same kernel with only lapic.c / smp.c compiled differently. TCG cannot do
# x2APIC; boot it under WHPX/KVM (tests/headless/smp_win.ps1).
build-x2/lapic.o: src/lapic.c $(HEADERS)
	mkdir -p build-x2
	$(CC) $(CFLAGS) -DLAPIC_FORCE_X2APIC -c $< -o $@
build-x2/smp.o: src/smp.c $(HEADERS)
	mkdir -p build-x2
	$(CC) $(CFLAGS) -DSMP_BOOT_SELFTEST -c $< -o $@
kanarchy-x2apic.bin: $(ASM_OBJ) $(COMMON_OBJ) $(DESKTOP_OBJ) src/desktop.o build-x2/lapic.o build-x2/smp.o linker-high.ld
	$(LD) $(LDFLAGS_HIGH) -o $@ $(ASM_OBJ) $(COMMON_OBJ) $(filter-out src/lapic.o src/smp.o,$(DESKTOP_OBJ)) src/desktop.o build-x2/lapic.o build-x2/smp.o
kanarchy-x2apic.iso: kanarchy-x2apic.bin
	rm -rf isodir-x2
	mkdir -p isodir-x2/boot/grub
	cp kanarchy-x2apic.bin isodir-x2/boot/kanarchy.bin
	printf 'set timeout=0\nset default=0\n\nmenuentry "KAnarchy OS" {\n    set gfxpayload=1920x1080x32\n    multiboot /boot/kanarchy.bin\n    boot\n}\n' > isodir-x2/boot/grub/grub.cfg
	grub-mkrescue -o $@ isodir-x2 2>/dev/null

# ---- Test build: ask the bootloader for another video mode ----
#   make kanarchy-res.iso VID_W=1366 VID_H=768
# (the kernel adapts to whatever mode GRUB hands over; tests/headless/test_resolution.py)
VID_W ?= 1920
VID_H ?= 1080
build-res/start.o: boot/start.asm
	mkdir -p build-res
	$(AS) $(ASFLAGS) -DVID_W=$(VID_W) -DVID_H=$(VID_H) $< -o $@
kanarchy-res.bin: build-res/start.o $(ASM_OBJ) $(COMMON_OBJ) $(DESKTOP_OBJ) src/desktop.o linker-high.ld
	$(LD) $(LDFLAGS_HIGH) -o $@ build-res/start.o $(filter-out boot/start.o,$(ASM_OBJ)) $(COMMON_OBJ) $(DESKTOP_OBJ) src/desktop.o
kanarchy-res.iso: kanarchy-res.bin
	rm -rf isodir-res
	mkdir -p isodir-res/boot/grub
	cp kanarchy-res.bin isodir-res/boot/kanarchy.bin
	printf 'set timeout=0\nset default=0\n\nmenuentry "KAnarchy OS" {\n    set gfxpayload=$(VID_W)x$(VID_H)x32\n    multiboot /boot/kanarchy.bin\n    boot\n}\n' > isodir-res/boot/grub/grub.cfg
	grub-mkrescue -o $@ isodir-res 2>/dev/null

# ---- Test build: a 1366x768 geometry inside QEMU's framebuffer (QEMU's VGA has no 1366-wide mode):
# exercises a width that is not a multiple of 8 and a tight pitch. tests/headless/test_odd_width.py
build-w1366/graphics.o: src/graphics.c $(HEADERS)
	mkdir -p build-w1366
	$(CC) $(CFLAGS) -DGFX_TEST_W=1366 -DGFX_TEST_H=768 -c $< -o $@
kanarchy-w1366.bin: $(ASM_OBJ) $(COMMON_OBJ) $(DESKTOP_OBJ) src/desktop.o build-w1366/graphics.o linker-high.ld
	$(LD) $(LDFLAGS_HIGH) -o $@ $(ASM_OBJ) $(COMMON_OBJ) $(filter-out src/graphics.o,$(DESKTOP_OBJ)) src/desktop.o build-w1366/graphics.o
kanarchy-w1366.iso: kanarchy-w1366.bin
	rm -rf isodir-w1366
	mkdir -p isodir-w1366/boot/grub
	cp kanarchy-w1366.bin isodir-w1366/boot/kanarchy.bin
	printf 'set timeout=0\nset default=0\n\nmenuentry "KAnarchy OS" {\n    set gfxpayload=1920x1080x32\n    multiboot /boot/kanarchy.bin\n    boot\n}\n' > isodir-w1366/boot/grub/grub.cfg
	grub-mkrescue -o $@ isodir-w1366 2>/dev/null

# ---- Laptop image: one USB/CD image for an old BIOS laptop (LAPTOP.md). GRUB cannot override the video mode a
# multiboot header asks for, so every resolution is its own kernel image (only start.o differs), next to the
# text-mode kernel with the hardware report.
define LAPTOP_RULE
build-laptop/start-$(1).o: boot/start.asm
	mkdir -p build-laptop
	$(AS) $(ASFLAGS) -DVID_W=$(2) -DVID_H=$(3) $$< -o $$@
build-laptop/kanarchy-$(1).bin: build-laptop/start-$(1).o $(ASM_OBJ) $(COMMON_OBJ) $(DESKTOP_OBJ) src/desktop.o linker-high.ld
	$(LD) $(LDFLAGS_HIGH) -o $$@ build-laptop/start-$(1).o $(filter-out boot/start.o,$(ASM_OBJ)) $(COMMON_OBJ) $(DESKTOP_OBJ) src/desktop.o
endef
$(eval $(call LAPTOP_RULE,1366x768,1366,768))
$(eval $(call LAPTOP_RULE,1280x800,1280,800))
$(eval $(call LAPTOP_RULE,1024x768,1024,768))
$(eval $(call LAPTOP_RULE,auto,0,0))
# Text-mode kernel for UEFI machines: no VGA text memory there, so the 80x25 cells are drawn on the GOP framebuffer
# (src/fbtext.c). Only vga/terminal/kernel see -DFBTEXT; the multiboot header asks GRUB for its automatic mode.
build-fbtext/start.o: boot/start.asm
	mkdir -p build-fbtext
	$(AS) $(ASFLAGS) -DTEXTMODE -DFBTEXT -DVID_W=0 -DVID_H=0 $< -o $@
build-fbtext/%.o: src/%.c $(HEADERS)
	mkdir -p build-fbtext
	$(CC) $(CFLAGS) -DFBTEXT -c $< -o $@
kanarchy-fbtext.bin: build-fbtext/start.o boot/isr.o $(COMMON_OBJ) build-fbtext/vga.o build-fbtext/terminal.o build-fbtext/kernel.o build-fbtext/fbtext.o src/shell.o src/hwinfo.o src/i915.o src/pcix.o src/acpi_pm.o src/text_stubs.o src/font_data.o
	$(LD) $(LDFLAGS) -o $@ $^

laptop: kanarchy-laptop.iso
kanarchy-laptop.iso: kanarchy-text.bin kanarchy-fbtext.bin build-laptop/kanarchy-1366x768.bin build-laptop/kanarchy-1280x800.bin build-laptop/kanarchy-1024x768.bin build-laptop/kanarchy-auto.bin boot/laptop-grub.cfg
	rm -rf isodir-laptop
	mkdir -p isodir-laptop/boot/grub
	cp kanarchy-text.bin isodir-laptop/boot/kanarchy-text.bin
	cp kanarchy-fbtext.bin isodir-laptop/boot/kanarchy-fbtext.bin
	for m in 1366x768 1280x800 1024x768 auto; do cp build-laptop/kanarchy-$$m.bin isodir-laptop/boot/kanarchy-$$m.bin; done
	cp boot/laptop-grub.cfg isodir-laptop/boot/grub/grub.cfg
	grub-mkrescue -o $@ isodir-laptop 2>/dev/null

# ---- Test build: the Intel graphics probe pointed at QEMU's standard VGA (vendor
# 1234) with a forced generation, so the PCI/BAR/MMIO/report code runs without an
# Intel GPU. tests/headless/test_i915_probe.py
build-i915/i915.o: src/i915.c $(HEADERS)
	mkdir -p build-i915
	$(CC) $(CFLAGS) -DI915_TEST_VENDOR=0x1234 -DI915_TEST_GEN=9 -c $< -o $@
kanarchy-i915test.bin: $(ASM_OBJ) $(COMMON_OBJ) $(DESKTOP_OBJ) src/desktop.o build-i915/i915.o linker-high.ld
	$(LD) $(LDFLAGS_HIGH) -o $@ $(ASM_OBJ) $(COMMON_OBJ) $(filter-out src/i915.o,$(DESKTOP_OBJ)) src/desktop.o build-i915/i915.o
kanarchy-i915test.iso: kanarchy-i915test.bin
	rm -rf isodir-i915
	mkdir -p isodir-i915/boot/grub
	cp kanarchy-i915test.bin isodir-i915/boot/kanarchy.bin
	printf 'set timeout=0\nset default=0\n\nmenuentry "KAnarchy OS" {\n    set gfxpayload=1920x1080x32\n    multiboot /boot/kanarchy.bin\n    boot\n}\n' > isodir-i915/boot/grub/grub.cfg
	grub-mkrescue -o $@ isodir-i915 2>/dev/null

# VP9 SSE2 interpolation kernels: like src/simd.c, the only media object the compiler may use XMM registers in (thread context only)
src/media/vp9_simd.o: src/media/vp9_simd.c src/media/vp9_simd.h
	$(CC) $(OJS_CFLAGS) -msse2 -fno-tree-vectorize -c $< -o $@

# Colour conversion + bilinear scaling for video frames: SSE2 as well (thread context only; runs on the decode worker too)
src/media/vcvt.o: src/media/vcvt.c src/media/vcvt.h
	$(CC) $(OJS_CFLAGS) -msse2 -fno-tree-vectorize -c $< -o $@
