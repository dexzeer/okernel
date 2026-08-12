# okernel Makefile

CC = gcc
AS = nasm
LD = ld

# Compiler flags
CFLAGS = -m32 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
         -nostartfiles -nodefaultlibs -fno-pic -fno-pie -mno-red-zone \
         -Wall -Wextra -Isrc

# Assembler flags
ASFLAGS = -f elf32

# Linker flags
LDFLAGS = -m elf_i386 -T linker.ld

# Common objects (no kernel.c, no desktop.c)
ASM_OBJ = boot/isr.o boot/start.o
COMMON_OBJ = src/gdt.o src/idt.o src/memory.o src/serial.o src/keyboard.o src/mouse.o

# Text mode objects
TEXT_OBJ = src/vga.o src/shell.o src/terminal.o

# Desktop mode objects
DESKTOP_OBJ = src/graphics.o src/window.o

.PHONY: all text desktop clean run debug

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

okernel-desktop.bin: $(ASM_OBJ) $(COMMON_OBJ) $(DESKTOP_OBJ) src/desktop.o
	$(LD) $(LDFLAGS) -o $@ $^

okernel-desktop.iso: okernel-desktop.bin
	mkdir -p isodir/boot/grub
	cp okernel-desktop.bin isodir/boot/okernel.bin
	echo 'set timeout=0' > isodir/boot/grub/grub.cfg
	echo 'set default=0' >> isodir/boot/grub/grub.cfg
	echo '' >> isodir/boot/grub/grub.cfg
	echo 'menuentry "okernel desktop" {' >> isodir/boot/grub/grub.cfg
	echo '    multiboot /boot/okernel.bin' >> isodir/boot/grub/grub.cfg
	echo '    boot' >> isodir/boot/grub/grub.cfg
	echo '}' >> isodir/boot/grub/grub.cfg
	grub-mkrescue -o $@ isodir 2>/dev/null

# ---- Assemble ----
%.o: %.asm
	$(AS) $(ASFLAGS) $< -o $@

# ---- Compile ----
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Run ----
run: text
	qemu-system-i386 -cdrom okernel-text.iso -boot d

run-desktop: desktop
	qemu-system-i386 -cdrom okernel-desktop.iso -boot d

debug: text
	qemu-system-i386 -cdrom okernel-text.iso -boot d -serial stdio

clean:
	rm -f $(ASM_OBJ) $(COMMON_OBJ) $(TEXT_OBJ) $(DESKTOP_OBJ) src/kernel.o src/desktop.o
	rm -f okernel-text.bin okernel-text.iso okernel-desktop.bin okernel-desktop.iso
	rm -rf isodir
