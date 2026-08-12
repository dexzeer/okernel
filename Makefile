# okernel Makefile

CC = gcc
AS = nasm
LD = ld

# Compiler flags for freestanding kernel (no standard library)
CFLAGS = -m32 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
         -nostartfiles -nodefaultlibs -fno-pic -fno-pie -mno-red-zone \
         -Wall -Wextra -Isrc

# Assembler flags
ASFLAGS = -f elf32

# Linker flags
LDFLAGS = -m elf_i386 -T linker.ld

# Source files
C_SRC = $(wildcard src/*.c)
ASM_SRC = $(wildcard boot/*.asm)
C_OBJ = $(C_SRC:.c=.o)
ASM_OBJ = $(ASM_SRC:.asm=.o)
OBJ = $(ASM_OBJ) $(C_OBJ)

# Output
KERNEL = okernel.bin
ISO = okernel.iso

.PHONY: all clean run iso

all: $(ISO)

# Assemble
%.o: %.asm
	$(AS) $(ASFLAGS) $< -o $@

# Compile
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Link kernel binary
$(KERNEL): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

# Create bootable ISO with GRUB
iso: $(KERNEL)
	mkdir -p isodir/boot/grub
	cp $(KERNEL) isodir/boot/okernel.bin
	echo 'set timeout=0' > isodir/boot/grub/grub.cfg
	echo 'set default=0' >> isodir/boot/grub/grub.cfg
	echo '' >> isodir/boot/grub/grub.cfg
	echo 'menuentry "okernel" {' >> isodir/boot/grub/grub.cfg
	echo '    multiboot /boot/okernel.bin' >> isodir/boot/grub/grub.cfg
	echo '    boot' >> isodir/boot/grub/grub.cfg
	echo '}' >> isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) isodir 2>/dev/null

# Build ISO as default
$(ISO): iso

# Run in QEMU
run: $(ISO)
	qemu-system-i386 -cdrom $(ISO)

# Run with serial output for debugging
debug: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -serial stdio -s

clean:
	rm -f $(OBJ) $(KERNEL) $(ISO)
	rm -rf isodir
