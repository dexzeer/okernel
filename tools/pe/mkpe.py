#!/usr/bin/env python3
"""Wrap a statically linked ELF64 (linked with tools/pe/pe.ld at a fixed base) into a PE32+ image.

    mkpe.py in.elf out.exe|out.dll [--dll] [--gui] [--base 0x10000000] [--imports imports.txt] [--export name,name,...]
            [--entry symbol] [--stack 1048576]

imports.txt: lines "DLLNAME.dll: func func func ..."; gen-imports (below) makes the assembler stubs for them. The IAT slots
come from the symbols __iat_<n>_<i> (n = index of the DLL in the file, i = index of the function).
"""
import struct, sys, argparse, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unwind
import rsrc

def read_elf(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 2, "not an ELF64 file"
    (shoff,) = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize)
        secs.append(dict(name=name, type=typ, flags=flags, addr=addr, off=off, size=size, link=link, info=info, entsize=entsize))
    stroff = secs[shstrndx]["off"]
    for s in secs:
        e = d.index(b"\0", stroff + s["name"])
        s["name"] = d[stroff + s["name"]:e].decode()
    syms = {}
    for s in secs:
        if s["type"] == 2:                                    # SHT_SYMTAB
            strtab = secs[s["link"]]
            for k in range(s["size"] // 24):
                nm, info, other, shndx, val, size = struct.unpack_from("<IBBHQQ", d, s["off"] + k * 24)
                e = d.index(b"\0", strtab["off"] + nm)
                syms[d[strtab["off"] + nm:e].decode()] = (val, shndx, info)
    entry, = struct.unpack_from("<Q", d, 0x18)
    # relocations kept by `ld -q`: (target section name, offset in it, type) for the sections whose 32-bit fields are RVAs
    for i, s in enumerate(secs):
        s["idx"] = i
    relocs = {}
    for s in secs:
        if s["type"] == 4:                                                   # SHT_RELA
            tgt = secs[s["info"]]
            for k in range(s["size"] // 24):
                off, info, addend = struct.unpack_from("<QQq", d, s["off"] + k * 24)
                relocs.setdefault(tgt["name"], []).append((off, info & 0xFFFFFFFF))
    return d, secs, syms, entry, relocs

def align(v, a): return (v + a - 1) // a * a

def parse_imports(path):
    out = []
    for line in open(path):
        line = line.split("#")[0].strip()
        if not line: continue
        dll, funcs = line.split(":", 1)
        names = []
        for tok in funcs.split():
            if tok.startswith("@"):                              # @file: every name listed in that file
                names += [l.strip() for l in open(tok[1:]) if l.strip()]
            else: names.append(tok)
        out.append((dll.strip(), names))
    return out

def gen_stubs(imports_path, out_path):
    """Assembler for the import stubs: `name: jmp *slot` in .stubs, the slots in .iat (one terminator slot per DLL)."""
    lines = ['    .section .stubs,"ax"']
    slots = []
    for n, (dll, funcs) in enumerate(parse_imports(imports_path)):
        for i, f in enumerate(funcs):
            lines.append(f'    .globl "{f}"\n"{f}":\n    jmp *__iat_{n}_{i}(%rip)')
        slots.append((n, len(funcs)))
    lines.append('    .section .iat,"aw"\n    .balign 8')
    for n, cnt in slots:
        for i in range(cnt):
            lines.append(f"    .globl __iat_{n}_{i}\n__iat_{n}_{i}:\n    .quad 0")
        lines.append(f"    .globl __iat_{n}_end\n__iat_{n}_end:\n    .quad 0")
    open(out_path, "w").write("\n".join(lines) + "\n")

def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "gen-imports":
        gen_stubs(sys.argv[2], sys.argv[3]); return
    ap = argparse.ArgumentParser()
    ap.add_argument("elf"); ap.add_argument("out")
    ap.add_argument("--dll", action="store_true"); ap.add_argument("--gui", action="store_true")
    ap.add_argument("--base", default="0x10000000"); ap.add_argument("--imports"); ap.add_argument("--export", default="")
    ap.add_argument("--entry", default=None); ap.add_argument("--stack", type=int, default=1 << 20)
    ap.add_argument("--name", default=None); ap.add_argument("--rsrc", default=None)
    a = ap.parse_args()
    base = int(a.base, 0)
    d, secs, syms, elf_entry, relocs = read_elf(a.elf)
    # allocated sections become PE sections (names kept, at most 8 characters)
    pes = []
    for s in secs:
        if not (s["flags"] & 2) or s["size"] == 0: continue            # SHF_ALLOC
        if s["name"] in (".eh_frame", ".sehfunc"): continue             # input of the unwind tables below, not shipped
        flags = 0x40000000                                              # readable
        if s["flags"] & 4: flags |= 0x20000000 | 0x20                   # executable code
        elif s["type"] == 8: flags |= 0xC0000000 | 0x80                 # .bss: read+write, uninitialised data
        else:
            flags |= 0x40 if True else 0
            if s["flags"] & 1: flags |= 0x80000000                      # writable
        data = None if s["type"] == 8 else bytearray(d[s["off"]:s["off"] + s["size"]])
        if s["name"] == ".sehdata" and data is not None:                # 32-bit relocations in hand written unwind data are RVAs
            for off, typ in relocs.get(".sehdata", []):
                o = off - s["addr"]
                struct.pack_into("<I", data, o, (struct.unpack_from("<I", data, o)[0] - base) & 0xFFFFFFFF)
        pes.append(dict(name=s["name"][:8], rva=s["addr"] - base, vsize=s["size"], data=None if data is None else bytes(data), flags=flags))
    # imports
    idata = bytearray()
    imports = parse_imports(a.imports) if a.imports else []
    idata_rva = max((p["rva"] + align(p["vsize"], 0x1000) for p in pes), default=0x1000)
    if imports:
        ndll = len(imports)
        desc_size = 20 * (ndll + 1)
        # layout: descriptors | per DLL: ILT (8*(n+1)) | names (dll name, hint/name entries)
        ilt_pos = []; name_pos = []
        p = desc_size
        for dll, funcs in imports:
            ilt_pos.append(p); p += 8 * (len(funcs) + 1)
        blob = bytearray(p)
        for k, (dll, funcs) in enumerate(imports):
            dname = len(blob); blob += dll.encode() + b"\0"; p = len(blob)
            if p & 1: blob += b"\0"; p += 1
            hn = []
            for f in funcs:
                if len(blob) & 1: blob += b"\0"
                hn.append(len(blob)); blob += struct.pack("<H", 0) + f.encode() + b"\0"
            for i, h in enumerate(hn):
                struct.pack_into("<Q", blob, ilt_pos[k] + 8 * i, idata_rva + h)
            first = syms["__iat_%d_0" % k][0] - base if funcs else syms["__iat_%d_end" % k][0] - base
            struct.pack_into("<IIIII", blob, 20 * k, idata_rva + ilt_pos[k], 0, 0, idata_rva + dname, first)
            # the IAT slots start as copies of the ILT (hint/name RVAs) so a loader that ignores the ILT still works
            for i, h in enumerate(hn):
                for pe in pes:
                    sa = first + 8 * i
                    if pe["rva"] <= sa < pe["rva"] + pe["vsize"] and pe["data"] is not None:
                        pe["data"] = bytearray(pe["data"]); struct.pack_into("<Q", pe["data"], sa - pe["rva"], idata_rva + h)
        idata = blob
        pes.append(dict(name=".idata", rva=idata_rva, vsize=len(idata), data=bytes(idata), flags=0xC0000040))
    # exports
    exp_rva = 0; exp_size = 0
    names = [n for n in a.export.split(",") if n]
    if names:
        nxt = max(p["rva"] + align(p["vsize"], 0x1000) for p in pes)
        dllname = (a.name or a.out.split("/")[-1]).encode() + b"\0"
        names.sort()
        n = len(names)
        hdr = 40
        funcs_o = hdr; names_o = funcs_o + 4 * n; ords_o = names_o + 4 * n; str_o = ords_o + 2 * n
        blob = bytearray(str_o)
        dll_o = len(blob); blob += dllname
        str_offs = []
        for nm in names:
            str_offs.append(len(blob)); blob += nm.encode() + b"\0"
        struct.pack_into("<IIHHIIIIIII", blob, 0, 0, 0, 0, 0, nxt + dll_o, 1, n, n, nxt + funcs_o, nxt + names_o, nxt + ords_o)
        for i, nm in enumerate(names):
            if nm not in syms: sys.exit("export %s not found in the ELF" % nm)
            struct.pack_into("<I", blob, funcs_o + 4 * i, syms[nm][0] - base)
            struct.pack_into("<I", blob, names_o + 4 * i, nxt + str_offs[i])
            struct.pack_into("<H", blob, ords_o + 2 * i, i)
        pes.append(dict(name=".edata", rva=nxt, vsize=len(blob), data=bytes(blob), flags=0x40000040))
        exp_rva, exp_size = nxt, len(blob)
    # unwind tables: .pdata (RUNTIME_FUNCTION) and .xdata (UNWIND_INFO), from the compiler's CFI plus hand written entries
    pd_rva = pd_size = 0
    eh = next((x for x in secs if x["name"] == ".eh_frame" and x["size"]), None)
    rt = []
    xd = b""
    if eh is not None:
        ents, xd, bad = unwind.build(d[eh["off"]:eh["off"] + eh["size"]], eh["addr"], base)
        if bad and os.environ.get("MKPE_UNWIND_VERBOSE"):
            names = {v[0]: k for k, v in syms.items() if v[2] & 0xF in (1, 2)}
            for b, why in bad: sys.stderr.write("unwind: no table for %s: %s\n" % (names.get(b, hex(b)), why))
        rt = [(b, e, u) for b, e, u in ents]
    user = next((x for x in secs if x["name"] == ".sehfunc" and x["size"]), None)
    user_rt = []
    if user is not None:
        raw = bytearray(d[user["off"]:user["off"] + user["size"]])
        for off, typ in relocs.get(".sehfunc", []):
            o = off - user["addr"]
            struct.pack_into("<I", raw, o, (struct.unpack_from("<I", raw, o)[0] - base) & 0xFFFFFFFF)
        for k in range(len(raw) // 12): user_rt.append(struct.unpack_from("<III", raw, 12 * k))
    if rt or user_rt:
        nxt = max(p["rva"] + align(p["vsize"], 0x1000) for p in pes)
        xd_rva = nxt
        if xd: pes.append(dict(name=".xdata", rva=xd_rva, vsize=len(xd), data=xd, flags=0x40000040)); nxt += align(len(xd), 0x1000)
        table = sorted([(b, e, xd_rva + u) for b, e, u in rt] + user_rt)
        pdata = b"".join(struct.pack("<III", *t) for t in table)
        pd_rva, pd_size = nxt, len(pdata)
        pes.append(dict(name=".pdata", rva=pd_rva, vsize=len(pdata), data=pdata, flags=0x40000040))
    # resources
    rs_rva = rs_size = 0
    if a.rsrc:
        nxt = max(p["rva"] + align(p["vsize"], 0x1000) for p in pes)
        rdata = rsrc.build(rsrc.parse(a.rsrc), nxt)
        pes.append(dict(name=".rsrc", rva=nxt, vsize=len(rdata), data=rdata, flags=0x40000040))
        rs_rva, rs_size = nxt, len(rdata)
    # headers
    entry_sym = a.entry or ("DllMain" if a.dll else None)
    entry_rva = (syms[entry_sym][0] - base) if entry_sym and entry_sym in syms else max(0, elf_entry - base)
    nsec = len(pes)
    hdr_size = align(0x40 + 4 + 20 + 240 + 40 * nsec, 0x200)
    foff = hdr_size
    for p in pes:
        p["foff"] = foff if p["data"] is not None else 0
        p["fsize"] = align(len(p["data"]), 0x200) if p["data"] is not None else 0
        foff += p["fsize"]
    size_of_image = align(max(p["rva"] + p["vsize"] for p in pes), 0x1000)
    out = bytearray(foff)
    out[0:2] = b"MZ"; struct.pack_into("<I", out, 0x3C, 0x40)
    out[0x40:0x44] = b"PE\0\0"
    chars = 0x22 | (0x2000 if a.dll else 0)                              # executable, large address aware, DLL
    struct.pack_into("<HHIIIHH", out, 0x44, 0x8664, nsec, 0, 0, 0, 240, chars)
    o = 0x58
    code_size = sum(p["fsize"] for p in pes if p["flags"] & 0x20)
    def put(fmt, off, *v): struct.pack_into(fmt, out, o + off, *v)
    put("<H", 0, 0x20B)                                                  # Magic: PE32+
    put("<BB", 2, 14, 0)
    put("<I", 4, code_size); put("<I", 16, entry_rva)
    put("<Q", 24, base)
    put("<II", 32, 0x1000, 0x200)                                        # section / file alignment
    put("<HH", 40, 6, 0); put("<HH", 48, 6, 0)                           # OS and subsystem version
    put("<I", 56, size_of_image); put("<I", 60, hdr_size)
    put("<HH", 68, 2 if a.gui else 3, 0)                                 # subsystem, DLL characteristics
    put("<QQQQ", 72, a.stack, 0x1000, 1 << 20, 0x1000)                   # stack and heap sizes
    put("<I", 108, 16)                                                   # number of data directories
    dd = o + 112
    if exp_rva: struct.pack_into("<II", out, dd, exp_rva, exp_size)
    if imports: struct.pack_into("<II", out, dd + 8, idata_rva, 20 * (len(imports) + 1))
    if rs_rva: struct.pack_into("<II", out, dd + 16, rs_rva, rs_size)
    if pd_rva: struct.pack_into("<II", out, dd + 24, pd_rva, pd_size)               # exception directory
    so = o + 240
    for i, p in enumerate(pes):
        nm = p["name"].encode()[:8].ljust(8, b"\0")
        struct.pack_into("<8sIIIIIIHHI", out, so + 40 * i, nm, p["vsize"], p["rva"], p["fsize"], p["foff"], 0, 0, 0, 0, p["flags"])
        if p["data"] is not None: out[p["foff"]:p["foff"] + len(p["data"])] = p["data"]
    open(a.out, "wb").write(out)

main()
