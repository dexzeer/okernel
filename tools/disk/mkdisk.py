#!/usr/bin/env python3
# Builds the installed-disk image (layout: src/part.h) from the desktop and recovery kernels, without root: GRUB's
# images come from grub-mkimage, the FAT file systems from mtools, the rest is written here.
#   python3 tools/disk/mkdisk.py OUT.img kanarchy-desktop.bin
# BIOS: GRUB's boot.img in the MBR, its core image in the gap before the first partition.
# UEFI: /EFI/BOOT/BOOTX64.EFI and BOOTIA32.EFI on the boot partition (type 0xEF, so firmware takes it as the ESP).
# Run in QEMU:  qemu-system-i386 -m 1024 -hda kanarchy-disk.img -vga std
import os, shutil, struct, subprocess, sys

MB = 2048                                           # sectors
P1 = (2048, 64 * MB, 0xEF, 'KANBOOT')               # boot: GRUB, the desktop kernel, grubenv
P2 = (P1[0] + P1[1], 32 * MB, 0x0E, 'KANRECOV')     # recovery (read-only): the known-good desktop kernel
P3 = (P2[0] + P2[1], 4 * MB, 0xDA, None)            # log + crash records (src/crash.h: 5121 sectors used)
P4 = (P3[0] + P3[1], 4 * MB, 0x4B, None)            # OKPFS1 (it uses at most 3825 sectors)
TOTAL = P4[0] + P4[1]

MODS_COMMON = 'part_msdos fat normal configfile search search_label loadenv test echo multiboot video all_video gfxterm reboot halt sleep minicmd ls cat'
MODS_PC = 'biosdisk vbe vga video_bochs video_cirrus ' + MODS_COMMON
MODS_EFI = 'efi_gop efi_uga ' + MODS_COMMON
GRUBLIB = '/usr/lib/grub'

def run(*a):
    subprocess.run(a, check=True)

def fat(path, part, files, dirs):
    if os.path.exists(path):
        os.unlink(path)
    spc = 4 if part[1] >= 64 * MB else 2              # FAT16 with ~32k clusters
    run('mformat', '-i', path, '-C', '-T', str(part[1]), '-h', '64', '-s', '32', '-H', str(part[0]), '-c', str(spc), '-v', part[3], '::')
    for d in dirs:
        run('mmd', '-i', path, '::' + d)
    for src, dst in files:
        run('mcopy', '-i', path, src, '::' + dst)

def main():
    out, desktop = sys.argv[1:3]
    work = 'build-disk'
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    early = os.path.join(work, 'early.cfg')
    with open(early, 'w') as f:
        f.write('search --no-floppy --label KANBOOT --set=root\nset prefix=($root)/boot/grub\n')
    core = os.path.join(work, 'core.img')
    run('grub-mkimage', '-O', 'i386-pc', '-o', core, '-p', '(hd0,msdos1)/boot/grub', '-c', early, *MODS_PC.split())
    efis = []
    for target, name in (('x86_64-efi', 'BOOTX64.EFI'), ('i386-efi', 'BOOTIA32.EFI')):
        if os.path.isdir(os.path.join(GRUBLIB, target)):
            p = os.path.join(work, name)
            run('grub-mkimage', '-O', target, '-o', p, '-p', '/boot/grub', '-c', early, *MODS_EFI.split())
            efis.append((p, '/EFI/BOOT/' + name))
    env = os.path.join(work, 'grubenv')
    run('grub-editenv', env, 'create')
    assert os.path.getsize(env) == 1024
    here = os.path.dirname(os.path.abspath(__file__))
    bootfat = os.path.join(work, 'boot.fat')
    fat(bootfat, P1, [(desktop, '/boot/kanarchy.bin'), (os.path.join(here, 'grub.cfg'), '/boot/grub/grub.cfg'), (env, '/boot/grub/grubenv')] + efis,
        ['/boot', '/boot/grub', '/EFI', '/EFI/BOOT'])
    recfat = os.path.join(work, 'recovery.fat')
    fat(recfat, P2, [(desktop, '/known/kanarchy.bin')], ['/known'])   # booted with `recovery` = the recovery desktop

    corebytes = open(core, 'rb').read()
    ncore = (len(corebytes) + 511) // 512
    if ncore >= P1[0] - 1:
        sys.exit('mkdisk: the GRUB core image does not fit before the first partition')
    corebytes = bytearray(corebytes + b'\0' * (ncore * 512 - len(corebytes)))
    # diskboot's block list (the last 12 bytes of the core's first sector): the rest of the core, from LBA 2
    struct.pack_into('<QHH', corebytes, 500, 2, ncore - 1, 0x820)

    mbr = bytearray(512)
    mbr[:440] = open(os.path.join(GRUBLIB, 'i386-pc', 'boot.img'), 'rb').read()[:440]
    struct.pack_into('<I', mbr, 440, 0x4B414E41)    # disk signature
    for i, (start, count, typ, _) in enumerate((P1, P2, P3, P4)):
        e = 446 + 16 * i
        mbr[e] = 0x80 if i == 0 else 0
        mbr[e + 1:e + 4] = b'\xfe\xff\xff'          # CHS: use the LBA fields
        mbr[e + 4] = typ
        mbr[e + 5:e + 8] = b'\xfe\xff\xff'
        struct.pack_into('<II', mbr, e + 8, start, count)
    mbr[510:512] = b'\x55\xaa'

    with open(out + '.tmp', 'wb') as f:
        f.truncate(TOTAL * 512)
        f.seek(0); f.write(mbr)
        f.seek(512); f.write(corebytes)
        for part, img in ((P1, bootfat), (P2, recfat)):
            f.seek(part[0] * 512)
            f.write(open(img, 'rb').read())
        # the log partition: klog header (src/klog.h) at its start, crash header (src/crash.h) at +4096
        f.seek(P3[0] * 512); f.write(b'KANARCHY-LOG-V1\0')
        f.seek((P3[0] + 4096) * 512); f.write(b'KANARCHY-CRASH1\0')
    os.replace(out + '.tmp', out)
    print(f'mkdisk: {out}, {TOTAL // MB} MB: boot p1 {P1[1] // MB} MB, recovery p2 {P2[1] // MB} MB, log p3, data p4; GRUB core {ncore} sectors, EFI: {", ".join(n for _, n in efis) or "none"}')

main()
