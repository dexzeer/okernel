#!/usr/bin/env python3
# Kernel symbol table for the panic screen (src/panic.c): reads `nm -n` output of the desktop kernel on stdin and writes
# the table that boot/ksyms.asm links into the .ksyms section (linker-high.ld puts it after .data, so only .bss moves
# between the first link without it and the final one; the code addresses it lists stay the same).
#   python3 tools/ksyms.py OUT          write the table
#   python3 tools/ksyms.py --check OUT  the final kernel's symbols must give the same table
# Layout (little endian): "KSYM", count, then count x (address, name offset) sorted by address, then the names (NUL
# terminated). Only code symbols (nm types T, t, W, w) between _kernel_start and the end of the code are kept.
import struct, sys

def table(lines):
    syms = []
    start = None
    for ln in lines:
        p = ln.split()
        if len(p) != 3:
            continue
        addr, typ, name = int(p[0], 16), p[1], p[2]
        if name == '_kernel_start':
            start = addr
        if typ in 'TtWw' and addr >= 0xC0000000:
            syms.append((addr, name))
    syms.sort()
    if start is not None:
        syms = [s for s in syms if s[0] >= start]
    names = bytearray()
    ents = bytearray()
    seen = set()
    for addr, name in syms:
        if addr in seen:            # aliases: keep the first name
            continue
        seen.add(addr)
        ents += struct.pack('<II', addr, len(names))
        names += name.encode() + b'\0'
    n = len(ents) // 8
    out = b'KSYM' + struct.pack('<I', n) + bytes(ents) + bytes(names)
    out += b'\0' * (-len(out) % 4)
    return out, n

def main():
    args = sys.argv[1:]
    check = args and args[0] == '--check'
    if check:
        args = args[1:]
    out, n = table(sys.stdin.read().splitlines())
    if check:
        old = open(args[0], 'rb').read()
        if old != out:
            sys.exit('ksyms: the code moved between the two links (the table no longer matches the kernel)')
        print(f'ksyms: {n} symbols, {len(out) // 1024} KB, verified')
        return
    open(args[0], 'wb').write(out)

main()
