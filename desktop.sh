qemu-system-i386 -cdrom okernel-desktop.iso -boot d -vga std -device rtl8139,netdev=net0 -netdev user,id=net0
