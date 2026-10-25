"""Resources for the PE files we build: a spec file becomes the .rsrc section.

Spec lines (blank lines and # comments ignored); ID is a number or a name; FILE is relative to the spec file:
    BITMAP      ID file.bmp        a .bmp file (the 14 byte file header is dropped): RT_BITMAP
    ICON        ID file.ico        an .ico file: RT_GROUP_ICON + one RT_ICON per image
    MENU        ID file.bin        a binary menu template (RT_MENU)
    DIALOG      ID file.bin        a binary dialog template (RT_DIALOG)
    ACCELERATOR ID file.bin        RT_ACCELERATOR
    RCDATA      ID file.bin        RT_RCDATA
    TYPE        N ID file.bin      any numeric resource type
    STRINGS     file.txt           lines "id text": RT_STRING blocks of 16 (text is UTF-16)
"""
import os, struct

TYPES = {"CURSOR": 1, "BITMAP": 2, "ICON": 3, "MENU": 4, "DIALOG": 5, "STRING": 6, "ACCELERATOR": 9, "RCDATA": 10,
         "MESSAGETABLE": 11, "GROUPCURSOR": 12, "GROUPICON": 14, "VERSION": 16, "MANIFEST": 24}


def parse(spec_path):
    base = os.path.dirname(os.path.abspath(spec_path))
    res = {}                                   # (type, name) -> bytes ; name is int or str
    def add(t, n, data): res[(t, n)] = data
    def rid(s): return int(s, 0) if s.lstrip("-").isdigit() or s.startswith("0x") else s.upper()
    next_icon = [1]
    for line in open(spec_path):
        line = line.split("#")[0].strip()
        if not line: continue
        w = line.split()
        kind = w[0].upper()
        if kind == "BITMAP":
            d = open(os.path.join(base, w[2]), "rb").read()
            add(2, rid(w[1]), d[14:])
        elif kind == "ICON":
            d = open(os.path.join(base, w[2]), "rb").read()
            n = struct.unpack_from("<H", d, 4)[0]
            grp = struct.pack("<HHH", 0, 1, n)
            for i in range(n):
                wd, ht, cc, rs, pl, bpp, size, off = struct.unpack_from("<BBBBHHII", d, 6 + 16 * i)
                ident = next_icon[0]; next_icon[0] += 1
                add(3, ident, d[off:off + size])
                grp += struct.pack("<BBBBHHIH", wd, ht, cc, rs, pl, bpp, size, ident)
            add(14, rid(w[1]), grp)
        elif kind in ("MENU", "DIALOG", "ACCELERATOR", "RCDATA", "MESSAGETABLE", "VERSION", "MANIFEST"):
            add(TYPES[kind], rid(w[1]), open(os.path.join(base, w[2]), "rb").read())
        elif kind == "TYPE":
            add(int(w[1], 0), rid(w[2]), open(os.path.join(base, w[3]), "rb").read())
        elif kind == "STRINGS":
            blocks = {}
            for l in open(os.path.join(base, w[1]), encoding="utf-8"):
                l = l.rstrip("\n")
                if not l.strip(): continue
                i, _, text = l.partition(" ")
                i = int(i)
                blocks.setdefault(i // 16 + 1, {})[i % 16] = text
            for b, items in blocks.items():
                data = b""
                for k in range(16):
                    t = items.get(k, "").encode("utf-16le")
                    data += struct.pack("<H", len(t) // 2) + t
                add(6, b, data)
        else:
            raise SystemExit("rsrc: unknown line %r" % line)
    return res


def build(res, rva):
    """-> section bytes (the resource directory tree and the data), with every data RVA relative to the section's own `rva`"""
    types = {}
    for (t, n), d in res.items(): types.setdefault(t, {})[n] = d
    def key(x): return (1, x) if isinstance(x, str) else (0, x)
    # layout: directories first, then name strings, then data entries, then the data
    dirs = []                                                  # [(offset, bytes-builder)]
    blob = bytearray()
    strings = {}
    def string_off(s):
        return strings.setdefault(s, None)
    # compute sizes
    def dir_size(nnamed, nid): return 16 + 8 * (nnamed + nid)
    type_keys = sorted(types, key=key)
    off = dir_size(sum(isinstance(k, str) for k in type_keys), sum(not isinstance(k, str) for k in type_keys))
    plan = []                                                  # per type: (type, name dir offset, [(name, lang dir offset, entry offset)])
    for t in type_keys:
        nm = sorted(types[t], key=key)
        name_dir_off = off
        off += dir_size(sum(isinstance(k, str) for k in nm), sum(not isinstance(k, str) for k in nm))
        plan.append([t, name_dir_off, nm, []])
    for p in plan:
        for n in p[2]:
            lang_dir_off = off
            off += dir_size(0, 1)
            p[3].append(lang_dir_off)
    data_entry_off = {}
    for p in plan:
        for n in p[2]:
            data_entry_off[(p[0], n)] = off
            off += 16
    str_base = off
    soff = str_base
    str_off = {}
    for p in plan:
        for k in [p[0]] + list(p[2]):
            if isinstance(k, str) and k not in str_off:
                str_off[k] = soff
                soff += 2 + 2 * len(k)
    data_base = (soff + 7) & ~7
    out = bytearray(data_base)
    def dirhead(o, nnamed, nid): struct.pack_into("<IIHHHH", out, o, 0, 0, 0, 0, nnamed, nid)
    def ident(k): return (0x80000000 | str_off[k]) if isinstance(k, str) else k
    nt = [k for k in type_keys if isinstance(k, str)]
    dirhead(0, len(nt), len(type_keys) - len(nt))
    for i, p in enumerate(plan):
        struct.pack_into("<II", out, 16 + 8 * i, ident(p[0]), 0x80000000 | p[1])
        nn = [k for k in p[2] if isinstance(k, str)]
        dirhead(p[1], len(nn), len(p[2]) - len(nn))
        for j, n in enumerate(p[2]):
            struct.pack_into("<II", out, p[1] + 16 + 8 * j, ident(n), 0x80000000 | p[3][j])
            dirhead(p[3][j], 0, 1)
            struct.pack_into("<II", out, p[3][j] + 16, 0x409, data_entry_off[(p[0], n)])
    for s, o in str_off.items():
        struct.pack_into("<H", out, o, len(s))
        out[o + 2:o + 2 + 2 * len(s)] = s.encode("utf-16le")
    for p in plan:
        for n in p[2]:
            d = types[p[0]][n]
            while len(out) % 8: out.append(0)
            struct.pack_into("<IIII", out, data_entry_off[(p[0], n)], rva + len(out), len(d), 0, 0)
            out += d
    return bytes(out)
