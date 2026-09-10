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
         -O2 -fno-strict-aliasing \
         -Wall -Wextra -DKERNEL -Isrc

# Assembler flags
ASFLAGS = -f elf32

# Linker flags (text = low link; desktop = high-half link at 0xC0100000)
LDFLAGS = -m elf_i386 -T linker.ld
LDFLAGS_HIGH = -m elf_i386 -T linker-high.ld

# Common objects (no kernel.c, no desktop.c). ORDER: start.o FIRST — its
# .text must land at 0xC0100030 (right after .multiboot) so _start's LOW
# alias (phys 0x100030+0x30) is where GRUB jumps. isr.o second.
ASM_OBJ = boot/start.o boot/isr.o
COMMON_OBJ = src/gdt.o src/idt.o src/memory.o src/serial.o src/keyboard.o src/mouse.o src/string.o src/syscall.o

# Text mode objects
TEXT_OBJ = src/vga.o src/shell.o src/terminal.o

# Desktop mode objects
# NOTE: -DKERNEL is set in CFLAGS below so the shared crypto/TLS source can
# switch its stdio logging to serial_printf and its RNG to the kernel CPRNG.
# NOTE: src/userland_seed.o embeds userland/gen_*.h bindata — it must rebuild
# whenever any gen header changes (the %.o rule only tracks HEADERS, and a
# stale seed silently ships last week's ELFs — bisected 2026-09-08 when a
# fresh forktest never reached the ISO).
USERLAND_GEN = $(wildcard userland/gen_*.h)
src/userland_seed.o: $(USERLAND_GEN)
DESKTOP_OBJ = src/graphics.o src/window.o src/paging.o src/process.o src/sched.o src/sys_proc.o src/elf.o src/spinlock.o src/ata.o src/pfs.o src/userland_seed.o src/net/pci.o src/net/e1000.o src/net/network.o src/filesystem.o src/editor.o src/okai.o src/html.o src/css.o \
              src/font_data.o \
              src/js/js_os.o src/js/js_var.o src/js/js_lex.o src/js/js_parse.o src/js/js_funcs.o src/js/js_math.o src/js/js_dom.o \
             src/crypto/sha256.o src/crypto/hmac.o src/crypto/hkdf.o \
             src/crypto/aead.o src/crypto/chacha20.o src/crypto/poly1305.o \
             src/crypto/x25519.o src/crypto/tls_record.o src/crypto/tls_handshake.o \
             src/crypto/tls_keysched.o src/crypto/tls_client.o src/crypto/rand.o \
             src/crypto/sha512.o src/crypto/der.o src/crypto/x509.o \
             src/crypto/rsa.o src/crypto/ec.o src/crypto/roots.o \
             src/crypto/certverify.o src/rtc.o \
             src/net/tls_net.o

.PHONY: all text desktop clean run debug host-tests host-tests-asan

# ---- Host tests (no QEMU; run from repo root — suites load tests/fixtures/*) ----
# Offline must-pass: tls_crypto, css, subres, pki, adversarial (fast -O2 build;
# the ASan build is 2-3x slower — see the commented line in the recipe).
# Informational (never gates): text_decode (5 pre-existing Cyrillic FAILs) and
# the live-network tls_client_test (needs internet to example.com:443).
HOST_CRYPTO_SRC = src/crypto/tls_client.c src/crypto/tls_record.c src/crypto/tls_handshake.c \
              src/crypto/tls_keysched.c src/crypto/sha256.c src/crypto/sha512.c \
              src/crypto/hmac.c src/crypto/hkdf.c src/crypto/aead.c \
              src/crypto/chacha20.c src/crypto/poly1305.c src/crypto/x25519.c \
              src/crypto/der.c src/crypto/x509.c src/crypto/rsa.c \
              src/crypto/ec.c src/crypto/certverify.c src/crypto/roots.c
host-tests:
	mkdir -p build-host
	gcc -m32 -O2 -Isrc/crypto -Isrc -o build-host/t_tls_crypto tests/test_tls_crypto.c src/crypto/sha256.c src/crypto/hmac.c src/crypto/hkdf.c src/crypto/aead.c src/crypto/chacha20.c src/crypto/poly1305.c src/crypto/x25519.c && ./build-host/t_tls_crypto | tail -n 2
	gcc -m32 -O2 -Isrc -Isrc/crypto -o build-host/t_rng tests/test_rng.c src/crypto/rand.c src/crypto/chacha20.c src/crypto/sha256.c && ./build-host/t_rng | tail -n 2
	gcc -m32 -O2 -Isrc/crypto -Isrc -o build-host/t_strict tests/test_crypto_strict.c $(HOST_CRYPTO_SRC) && ./build-host/t_strict | tail -n 2
	gcc -m32 -O2 -DKERNEL=0 -Isrc -o build-host/t_css tests/test_css.c src/css.c src/html.c && ./build-host/t_css | tail -n 2
	gcc -m32 -O2 -DKERNEL=0 -Isrc -o build-host/t_subres tests/test_subres.c src/html.c src/css.c && ./build-host/t_subres | tail -n 2
	gcc -m32 -O2 -Isrc/crypto -Isrc -o build-host/t_pki tests/test_pki.c $(HOST_CRYPTO_SRC) && ./build-host/t_pki | tail -n 2
	gcc -m32 -O2 -Isrc/crypto -Isrc -o build-host/t_adv tests/test_adversarial.c $(HOST_CRYPTO_SRC) && timeout 300 ./build-host/t_adv | tail -n 3
	-gcc -m32 -DKERNEL=0 -Isrc -o build-host/t_td tests/test_text_decode.c src/html.c && ./build-host/t_td | tail -n 2; echo "(text_decode: 5 pre-existing Cyrillic FAILs expected)"
	-gcc -m32 -O2 -Isrc/crypto -Isrc -o build-host/t_tls_live tests/test_tls_client.c $(HOST_CRYPTO_SRC) && timeout 60 ./build-host/t_tls_live | tail -n 3; echo "(tls_client_test: needs internet; SKIP if unreachable)"

# Sanitizer run of the adversarial suite (review P0 infra): every parser,
# flight, and mock path under ASan+UBSan. Slow (~10 min, RSA-heavy) — run
# after crypto/TLS changes, not on every edit.
host-tests-asan:
	mkdir -p build-host
	gcc -m32 -O1 -g -fsanitize=address,undefined -Isrc/crypto -Isrc -o build-host/t_adv_asan tests/test_adversarial.c $(HOST_CRYPTO_SRC) && timeout 590 ./build-host/t_adv_asan | tail -n 3

all: text

# ---- Text mode build ----
text: okernel-text.iso

okernel-text.bin: $(ASM_OBJ) $(COMMON_OBJ) $(TEXT_OBJ) src/kernel.o
	$(LD) $(LDFLAGS) -o $@ $^

okernel-text.iso: okernel-text.bin
	mkdir -p isodir/boot/grub
	cp okernel-text.bin isodir/boot/okernel.bin
	echo 'set timeout=0' > isodir/boot/grub/grub.cfg
	echo 'set default=0' >> isodir/boot/grub/grub.cfg
	echo '' >> isodir/boot/grub/grub.cfg
	echo 'menuentry "okernel text" {' >> isodir/boot/grub/grub.cfg
	echo '    multiboot /boot/okernel.bin' >> isodir/boot/grub/grub.cfg
	echo '    boot' >> isodir/boot/grub/grub.cfg
	echo '}' >> isodir/boot/grub/grub.cfg
	grub-mkrescue -o $@ isodir 2>/dev/null

# ---- Desktop mode build ----
desktop: okernel-desktop.iso

okernel-desktop.bin: $(ASM_OBJ) $(COMMON_OBJ) $(DESKTOP_OBJ) src/desktop.o linker-high.ld
	$(LD) $(LDFLAGS_HIGH) -o $@ $(ASM_OBJ) $(COMMON_OBJ) $(DESKTOP_OBJ) src/desktop.o

okernel-desktop.iso: okernel-desktop.bin
	rm -rf isodir
	mkdir -p isodir/boot/grub
	cp okernel-desktop.bin isodir/boot/okernel.bin
	echo 'set timeout=0' > isodir/boot/grub/grub.cfg
	echo 'set default=0' >> isodir/boot/grub/grub.cfg
	echo '' >> isodir/boot/grub/grub.cfg
	echo 'menuentry "okernel desktop" {' >> isodir/boot/grub/grub.cfg
	echo '    set gfxpayload=1920x1080x32' >> isodir/boot/grub/grub.cfg
	echo '    multiboot /boot/okernel.bin' >> isodir/boot/grub/grub.cfg
	echo '    boot' >> isodir/boot/grub/grub.cfg
	echo '}' >> isodir/boot/grub/grub.cfg
	grub-mkrescue -o $@ isodir 2>/dev/null

# ---- Assemble ----
%.o: %.asm
	$(AS) $(ASFLAGS) $< -o $@

# ---- Compile ----
# Objects depend on all headers — FONT_SCALE/layout constants live in
# graphics.h/window.h and stale objects with mixed metrics garble rendering
HEADERS := $(wildcard src/*.h) $(wildcard src/net/*.h)
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# JS engine files need the freestanding shim (kmalloc/serial_printf + no libc)
src/js/%.o: src/js/%.c $(HEADERS)
	$(CC) $(CFLAGS) -DJS_KERNEL -c $< -o $@

# ---- Run ----
run: text
	qemu-system-i386 -cdrom okernel-text.iso -boot d

run-desktop: desktop
	qemu-system-i386 -cdrom okernel-desktop.iso -boot d -vga std -device e1000,netdev=net0 -netdev user,id=net0 -fullscreen

debug: text
	qemu-system-i386 -cdrom okernel-text.iso -boot d -serial stdio

clean:
	rm -f $(ASM_OBJ) $(COMMON_OBJ) $(TEXT_OBJ) $(DESKTOP_OBJ) src/kernel.o src/desktop.o
	rm -f okernel-text.bin okernel-text.iso okernel-desktop.bin okernel-desktop.iso
	rm -rf isodir
