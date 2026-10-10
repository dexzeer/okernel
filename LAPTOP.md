# Running KAnarchy on an old laptop (UEFI or legacy BIOS)

`make laptop` builds `kanarchy-laptop.iso` (about 33 MB): one image with a boot menu, made for a laptop with a
PS/2 keyboard and touchpad and a 1366x768 panel. It boots both ways: **UEFI** (32-bit and 64-bit firmware: the image
carries `BOOTIA32.EFI` and `BOOTX64.EFI`) and legacy BIOS. Nothing here has run on real hardware yet; the first goal
is to find out what that machine is.

## Putting it on a USB stick (or a CD)

The ISO is a hybrid image, so it can be written straight to a USB stick. This erases the stick.

* **Windows:** Rufus, choose the ISO, and when it asks pick **"DD Image mode"** (not "ISO mode").
  Or balenaEtcher: choose the ISO and the stick.
* **Linux:** `sudo dd if=kanarchy-laptop.iso of=/dev/sdX bs=4M status=progress` (check X with `lsblk`).
* **CD/DVD:** burn it as an ordinary ISO image.

Boot from it: press the boot-menu key at power-on (usually F12, F9, Esc or F10) and pick the stick
(**"UEFI: USB DISK"** on a UEFI machine). If the firmware has **Secure Boot** on, turn it off in the setup (F2 / Del):
GRUB on the stick is not signed for it.

## The menu

On a UEFI machine the menu starts on entry 1b by itself after 30 seconds.

1. **Hardware report (text mode, legacy BIOS only).** Real VGA text mode: only a BIOS machine has one.
1b. **Hardware report on the screen (UEFI and BIOS) - START HERE.** The same report drawn on the graphics screen.
   It prints the CPU and the graphics chip by itself. Photograph the screen. Then type these commands and
   photograph each result: `pci`, `memmap`, `acpi`, and `pcidump 0:2.0` (the Intel graphics chip, if `pci` lists it
   as 00:02.0). `all` prints everything (Page Up / Page Down scroll).
2. **Desktop 1366x768** - the real thing at the panel's size. Try this second.
2n. **Desktop 1366x768, USB driver off** - the same as 2 without the USB controller driver, in case 2 hangs at boot.
   A **USB mouse** works in the desktop entries (plug it in before or after boot). The shell command `usb` says whether
   the controller started and how many mouse reports arrived.
3. **Desktop 1280x800**, 4. **1024x768**, 5. **let the video firmware choose** - fallbacks if 2 shows nothing or a
   scrambled picture. If a mode is not usable the screen shows a one-line message and the machine stops (BIOS) or
   GRUB says it could not set the mode; reboot and pick another entry.
6. **List the video modes this laptop offers** - GRUB prints every graphics mode (with bits per pixel) and the
   panel's preferred mode. It pauses after each screen: press Space for the next. Photograph every screen: the
   kernel needs a 32 bits-per-pixel mode.

## What to send back

Photos of: entry 1b as it appears at boot, `pci`, `acpi`, `memmap`, `pcidump 0:2.0`, and the screen of entry 6; and
what entries 2-5 do (picture, black screen, message, reboot).

## Known limits on such a machine

* The built-in keyboard works (PS/2, also under UEFI: the embedded controller emulates it). This laptop's touchpad is
  I2C and has no driver yet: use a USB mouse. USB keyboards are not supported.
* No Wi-Fi, no hard disk access, no sound codecs tested: the network driver is for an Intel
  e1000 card, which a laptop will not have. The browser will not reach the internet.
* The kernel needs no SSE2 and runs on a Pentium III or later; 256 MB of RAM or more. Under UEFI the memory map
  of the firmware is used (holes included); a framebuffer above 4 GB cannot be used by this 32-bit kernel.
