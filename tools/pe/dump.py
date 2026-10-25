#!/usr/bin/env python3
"""List what a PE32+ file needs: sections, entry point, imported DLLs and functions.

    python3 tools/pe/dump.py /mnt/c/Windows/System32/hostname.exe
"""
import struct, sys

def parse(path):
    d = open(path, "rb").read()
    e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
    assert d[e_lfanew:e_lfanew + 4] == b"PE\0\0", "not a PE file"
    machine, nsec, _, _, _, optsize, chars = struct.unpack_from("<HHIIIHH", d, e_lfanew + 4)
    opt = e_lfanew + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    assert magic == 0x20B, "not PE32+"
    entry, = struct.unpack_from("<I", d, opt + 16)
    base, = struct.unpack_from("<Q", d, opt + 24)
    sect_align, file_align = struct.unpack_from("<II", d, opt + 32)
    size_image, size_headers = struct.unpack_from("<II", d, opt + 56)
    subsystem, dllchars = struct.unpack_from("<HH", d, opt + 68)
    ndir, = struct.unpack_from("<I", d, opt + 108)
    dirs = [struct.unpack_from("<II", d, opt + 112 + 8 * i) for i in range(ndir)]
    secs = []
    so = opt + optsize
    for i in range(nsec):
        name, vsize, va, rsize, roff = struct.unpack_from("<8sIIII", d, so + 40 * i)
        flags, = struct.unpack_from("<I", d, so + 40 * i + 36)
        secs.append((name.rstrip(b"\0").decode(), va, vsize, roff, rsize, flags))
    def rva2off(rva):
        for n, va, vs, ro, rs, fl in secs:
            if va <= rva < va + max(vs, rs): return ro + rva - va
        return None
    def cstr(off):
        e = d.index(b"\0", off)
        return d[off:e].decode(errors="replace")
    imports = []
    if ndir > 1 and dirs[1][0]:
        o = rva2off(dirs[1][0])
        while o is not None:
            ilt, _, _, name, iat = struct.unpack_from("<IIIII", d, o)
            if not name: break
            dll = cstr(rva2off(name))
            funcs = []
            t = rva2off(ilt or iat)
            while True:
                v, = struct.unpack_from("<Q", d, t)
                if not v: break
                if v >> 63: funcs.append("#%d" % (v & 0xFFFF))
                else: funcs.append(cstr(rva2off(v & 0x7FFFFFFF) + 2))
                t += 8
            imports.append((dll, funcs))
            o += 20
    return dict(machine=machine, base=base, entry=entry, subsystem=subsystem, dllchars=dllchars, secs=secs,
                size_image=size_image, imports=imports, dirs=dirs, chars=chars)

if __name__ == "__main__":
    p = parse(sys.argv[1])
    print("machine %#x  base %#x  entry rva %#x  subsystem %d  image %#x  dllchars %#x" % (p["machine"], p["base"], p["entry"], p["subsystem"], p["size_image"], p["dllchars"]))
    for s in p["secs"]: print("  section %-8s rva %#8x vsize %#8x file %#8x+%#x flags %#x" % s)
    names = {1: "export", 2: "import", 3: "resource", 5: "reloc", 9: "tls", 10: "loadcfg", 12: "iat", 13: "delay", 14: "clr"}
    for i, (rva, size) in enumerate(p["dirs"]):
        if rva: print("  dir %-8s rva %#x size %#x" % (names.get(i, i), rva, size))
    total = 0
    for dll, funcs in p["imports"]:
        total += len(funcs)
        print("import %s (%d)" % (dll, len(funcs)))
        if "-v" in sys.argv: print("   " + " ".join(funcs))
    print("total imported functions:", total)
