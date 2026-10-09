#!/usr/bin/env python3
"""Generate src/ojs/unicode_data.c from the Unicode Character Database.

    python3 tools/gen_unicode.py <ucd-dir> > src/ojs/unicode_data.c

UCD files used (https://www.unicode.org/Public/UCD/latest/ucd/):
UnicodeData.txt DerivedCoreProperties.txt PropList.txt CaseFolding.txt
SpecialCasing.txt Scripts.txt ScriptExtensions.txt PropertyValueAliases.txt
DerivedNormalizationProps.txt CompositionExclusions.txt emoji-data.txt
DerivedBinaryProperties.txt

Everything is emitted as sorted [start, end] range lists (binary search at
run time) or sorted (code point, value) pairs.
"""
import sys, os, re

D = sys.argv[1]
def rd(name):
    with open(os.path.join(D, name), encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                yield [x.strip() for x in line.split(";")]

def parse_range(s):
    if ".." in s:
        a, b = s.split("..")
        return int(a, 16), int(b, 16)
    v = int(s, 16)
    return v, v

def to_ranges(cps):
    cps = sorted(set(cps))
    out = []
    for c in cps:
        if out and out[-1][1] == c - 1:
            out[-1][1] = c
        else:
            out.append([c, c])
    return out

def merge(ranges):
    rs = sorted(ranges)
    out = []
    for a, b in rs:
        if out and a <= out[-1][1] + 1:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out

# ---------------------------------------------------------------- UnicodeData

gc_of = {}          # cp -> category
simple_upper, simple_lower = {}, {}
decomp = {}         # cp -> (compat, [cps])
ccc = {}
first = None
for f in rd("UnicodeData.txt"):
    cp = int(f[0], 16)
    name, cat = f[1], f[2]
    if name.endswith(", First>"):
        first = cp
        continue
    if name.endswith(", Last>"):
        for c in range(first, cp + 1):
            gc_of[c] = cat
        continue
    gc_of[cp] = cat
    if f[3] != "0":
        ccc[cp] = int(f[3])
    if f[5]:
        parts = f[5].split()
        compat = parts[0].startswith("<")
        if compat:
            parts = parts[1:]
        decomp[cp] = (compat, [int(x, 16) for x in parts])
    if f[12]:
        simple_upper[cp] = int(f[12], 16)
    if f[13]:
        simple_lower[cp] = int(f[13], 16)

gc_ranges = {}
for cp, cat in gc_of.items():
    gc_ranges.setdefault(cat, []).append(cp)
gc_ranges = {k: to_ranges(v) for k, v in gc_ranges.items()}
assigned = to_ranges(gc_of.keys())
gc_ranges["Cn"] = []   # computed at run time as the complement of assigned
groups = {"L": ["Lu", "Ll", "Lt", "Lm", "Lo"], "LC": ["Lu", "Ll", "Lt"], "M": ["Mn", "Mc", "Me"],
          "N": ["Nd", "Nl", "No"], "P": ["Pc", "Pd", "Ps", "Pe", "Pi", "Pf", "Po"], "S": ["Sm", "Sc", "Sk", "So"],
          "Z": ["Zs", "Zl", "Zp"], "C": ["Cc", "Cf", "Cs", "Co"]}   # C also includes Cn (run time)
for g, subs in groups.items():
    rr = []
    for s in subs:
        rr += gc_ranges.get(s, [])
    gc_ranges[g] = merge(rr)

# ---------------------------------------------------------------- binary properties

binprops = {}
def add_props(fname):
    for f in rd(fname):
        if len(f) < 2:
            continue
        a, b = parse_range(f[0])
        binprops.setdefault(f[1], []).append([a, b])

add_props("DerivedCoreProperties.txt")
add_props("PropList.txt")
add_props("emoji-data.txt")
add_props("DerivedBinaryProperties.txt")
add_props("DerivedNormalizationProps.txt")
# CaseFolding-derived and others ECMAScript requires that UCD derives elsewhere
binprops = {k: merge(v) for k, v in binprops.items()}

# ECMA-262 table "Binary Unicode property aliases" (names we accept)
ES_BINARY = ["ASCII", "ASCII_Hex_Digit", "Alphabetic", "Any", "Assigned", "Bidi_Control", "Bidi_Mirrored",
    "Case_Ignorable", "Cased", "Changes_When_Casefolded", "Changes_When_Casemapped", "Changes_When_Lowercased",
    "Changes_When_NFKC_Casefolded", "Changes_When_Titlecased", "Changes_When_Uppercased", "Dash",
    "Default_Ignorable_Code_Point", "Deprecated", "Diacritic", "Emoji", "Emoji_Component", "Emoji_Modifier",
    "Emoji_Modifier_Base", "Emoji_Presentation", "Extended_Pictographic", "Extender", "Grapheme_Base",
    "Grapheme_Extend", "Hex_Digit", "IDS_Binary_Operator", "IDS_Trinary_Operator", "ID_Continue", "ID_Start",
    "Ideographic", "Join_Control", "Logical_Order_Exception", "Lowercase", "Math", "Noncharacter_Code_Point",
    "Pattern_Syntax", "Pattern_White_Space", "Quotation_Mark", "Radical", "Regional_Indicator",
    "Sentence_Terminal", "Soft_Dotted", "Terminal_Punctuation", "Unified_Ideograph", "Uppercase",
    "Variation_Selector", "White_Space", "XID_Continue", "XID_Start"]
BIN_ALIASES = {"ASCII": [], "ASCII_Hex_Digit": ["AHex"], "Alphabetic": ["Alpha"], "Any": [], "Assigned": [],
    "Bidi_Control": ["Bidi_C"], "Bidi_Mirrored": ["Bidi_M"], "Case_Ignorable": ["CI"], "Cased": [],
    "Changes_When_Casefolded": ["CWCF"], "Changes_When_Casemapped": ["CWCM"], "Changes_When_Lowercased": ["CWL"],
    "Changes_When_NFKC_Casefolded": ["CWKCF"], "Changes_When_Titlecased": ["CWT"], "Changes_When_Uppercased": ["CWU"],
    "Dash": [], "Default_Ignorable_Code_Point": ["DI"], "Deprecated": ["Dep"], "Diacritic": ["Dia"],
    "Emoji": [], "Emoji_Component": ["EComp"], "Emoji_Modifier": ["EMod"], "Emoji_Modifier_Base": ["EBase"],
    "Emoji_Presentation": ["EPres"], "Extended_Pictographic": ["ExtPict"], "Extender": ["Ext"],
    "Grapheme_Base": ["Gr_Base"], "Grapheme_Extend": ["Gr_Ext"], "Hex_Digit": ["Hex"],
    "IDS_Binary_Operator": ["IDSB"], "IDS_Trinary_Operator": ["IDST"], "ID_Continue": ["IDC"], "ID_Start": ["IDS"],
    "Ideographic": ["Ideo"], "Join_Control": ["Join_C"], "Logical_Order_Exception": ["LOE"], "Lowercase": ["Lower"],
    "Math": [], "Noncharacter_Code_Point": ["NChar"], "Pattern_Syntax": ["Pat_Syn"], "Pattern_White_Space": ["Pat_WS"],
    "Quotation_Mark": ["QMark"], "Radical": [], "Regional_Indicator": ["RI"], "Sentence_Terminal": ["STerm"],
    "Soft_Dotted": ["SD"], "Terminal_Punctuation": ["Term"], "Unified_Ideograph": ["UIdeo"], "Uppercase": ["Upper"],
    "Variation_Selector": ["VS"], "White_Space": ["space"], "XID_Continue": ["XIDC"], "XID_Start": ["XIDS"]}
binprops["ASCII"] = [[0, 0x7F]]
binprops["Any"] = [[0, 0x10FFFF]]
binprops["Assigned"] = assigned
if "Bidi_Mirrored" not in binprops:
    bm = []
    for f in rd("UnicodeData.txt"):
        pass
    binprops["Bidi_Mirrored"] = []
# Bidi_Mirrored lives in UnicodeData field 9
bm = []
first = None
for f in rd("UnicodeData.txt"):
    cp = int(f[0], 16)
    if f[9] == "Y":
        bm.append(cp)
binprops["Bidi_Mirrored"] = to_ranges(bm)

# ---------------------------------------------------------------- scripts

scripts = {}
for f in rd("Scripts.txt"):
    a, b = parse_range(f[0])
    scripts.setdefault(f[1], []).append([a, b])
scripts = {k: merge(v) for k, v in scripts.items()}
# value aliases (sc short <-> long)
sc_alias = {}
gc_alias = {}
for f in rd("PropertyValueAliases.txt"):
    if f[0] == "sc":
        sc_alias[f[2]] = [f[1]] + f[3:]
    elif f[0] == "gc":
        gc_alias[f[1]] = [f[2]] + f[3:]
short_of = {}
for longname, als in sc_alias.items():
    for a in als:
        short_of[a] = longname
scx = {k: [list(r) for r in v] for k, v in scripts.items()}
for f in rd("ScriptExtensions.txt"):
    a, b = parse_range(f[0])
    for s in f[1].split():
        lng = short_of.get(s, s)
        scx.setdefault(lng, []).append([a, b])
# the code points listed in ScriptExtensions have scx = that list (not their sc)
listed = []
for f in rd("ScriptExtensions.txt"):
    listed.append(parse_range(f[0]))
def subtract(ranges, holes):
    out = []
    for a, b in ranges:
        segs = [[a, b]]
        for ha, hb in holes:
            nxt = []
            for x, y in segs:
                if hb < x or ha > y:
                    nxt.append([x, y])
                    continue
                if x < ha:
                    nxt.append([x, ha - 1])
                if hb < y:
                    nxt.append([hb + 1, y])
            segs = nxt
        out += segs
    return out
listed_ranges = merge([list(r) for r in listed])
for k in list(scx.keys()):
    base = subtract(scripts.get(k, []), listed_ranges)
    ext = []
    for f in rd("ScriptExtensions.txt"):
        a, b = parse_range(f[0])
        if k in [short_of.get(s, s) for s in f[1].split()]:
            ext.append([a, b])
    scx[k] = merge(base + ext)

# ---------------------------------------------------------------- case data

special_upper, special_lower = {}, {}
for f in rd("SpecialCasing.txt"):
    if len(f) >= 5 and f[4]:
        continue   # conditional mappings: final sigma etc. handled in code
    cp = int(f[0], 16)
    lower = [int(x, 16) for x in f[1].split()]
    upper = [int(x, 16) for x in f[3].split()]
    if lower != [cp] and len(lower) > 1:
        special_lower[cp] = lower
    if upper != [cp] and len(upper) > 1:
        special_upper[cp] = upper
fold_simple = {}
for f in rd("CaseFolding.txt"):
    if f[1] in ("C", "S"):
        fold_simple[int(f[0], 16)] = int(f[2], 16)

# ---------------------------------------------------------------- normalization

excl = set()
for f in rd("CompositionExclusions.txt"):
    excl.add(int(f[0], 16))
for f in rd("DerivedNormalizationProps.txt"):
    if f[1] == "Full_Composition_Exclusion":
        a, b = parse_range(f[0])
        for c in range(a, b + 1):
            excl.add(c)
compose = []
for cp, (compat, seq) in decomp.items():
    if not compat and len(seq) == 2 and cp not in excl and ccc.get(cp, 0) == 0:
        compose.append((seq[0], seq[1], cp))
compose.sort()

# ---------------------------------------------------------------- emit

out = []
def w(s=""):
    out.append(s)

w("// unicode_data.c — GENERATED by tools/gen_unicode.py from the Unicode")
w("// Character Database. Do not edit; regenerate.")
w('#include "unicode.h"')
w()
tables = []
def emit_ranges(cname, ranges):
    flat = []
    for a, b in ranges:
        flat += [a, b]
    w(f"static const uint32_t {cname}[] = {{")
    for i in range(0, len(flat), 12):
        w("    " + ", ".join("0x%X" % v for v in flat[i:i + 12]) + ",")
    if not flat:
        w("    0")
    w("};")
    return cname, len(ranges)

def cid(s):
    return re.sub(r"[^A-Za-z0-9_]", "_", s)

emit_ranges("R_ID_START", binprops["ID_Start"])
emit_ranges("R_ID_CONTINUE", binprops["ID_Continue"])
w("const struct urange_set uni_id_start = { R_ID_START, %d };" % len(binprops["ID_Start"]))
w("const struct urange_set uni_id_continue = { R_ID_CONTINUE, %d };" % len(binprops["ID_Continue"]))
w()

# general categories
gc_entries = []
for cat in sorted(gc_ranges.keys()):
    nm, n = emit_ranges("R_GC_" + cid(cat), gc_ranges[cat])
    gc_entries.append((cat, nm, n))
w("const struct uprop uni_gc[] = {")
for cat, nm, n in gc_entries:
    names = [cat] + gc_alias.get(cat, [])
    w('    { "%s", { %s, %d } },' % ("|".join(names), nm, n))
w("    { 0, { 0, 0 } }")
w("};")
w()

# binary properties
bp_entries = []
for p in ES_BINARY:
    nm, n = emit_ranges("R_BP_" + cid(p), binprops.get(p, []))
    bp_entries.append((p, nm, n))
w("const struct uprop uni_binary[] = {")
for p, nm, n in bp_entries:
    names = [p] + BIN_ALIASES.get(p, [])
    w('    { "%s", { %s, %d } },' % ("|".join(names), nm, n))
w("    { 0, { 0, 0 } }")
w("};")
w()

# scripts / script extensions
for label, data in (("SC", scripts), ("SCX", scx)):
    ents = []
    for k in sorted(data.keys()):
        nm, n = emit_ranges("R_%s_%s" % (label, cid(k)), data[k])
        ents.append((k, nm, n))
    w("const struct uprop uni_%s[] = {" % label.lower())
    for k, nm, n in ents:
        names = [k] + sc_alias.get(k, [])
        names = list(dict.fromkeys(names))
        w('    { "%s", { %s, %d } },' % ("|".join(names), nm, n))
    w("    { 0, { 0, 0 } }")
    w("};")
    w()

def emit_pairs(cname, d):
    items = sorted(d.items())
    w(f"static const uint32_t {cname}[] = {{")
    flat = []
    for a, b in items:
        flat += [a, b]
    for i in range(0, len(flat), 12):
        w("    " + ", ".join("0x%X" % v for v in flat[i:i + 12]) + ",")
    w("};")
    w(f"const struct upairs uni_{cname.lower()} = {{ {cname}, {len(items)} }};")

emit_pairs("UPPER", simple_upper)
emit_pairs("LOWER", simple_lower)
emit_pairs("FOLD", fold_simple)
w()

def emit_multi(cname, d):
    items = sorted(d.items())
    w(f"static const uint32_t {cname}[] = {{   // cp, n, c1, c2, c3")
    for cp, seq in items:
        s = seq + [0] * (3 - len(seq))
        w("    0x%X, %d, 0x%X, 0x%X, 0x%X," % (cp, len(seq), s[0], s[1], s[2]))
    w("};")
    w(f"const struct umulti uni_{cname.lower()} = {{ {cname}, {len(items)} }};")
emit_multi("SPECIAL_UPPER", special_upper)
emit_multi("SPECIAL_LOWER", special_lower)
w()

# decompositions: cp, flags(compat<<7 | len), offset into DECOMP_SEQ
seq = []
dent = []
for cp in sorted(decomp):
    compat, s = decomp[cp]
    dent.append((cp, (1 if compat else 0) << 7 | len(s), len(seq)))
    seq += s
w("static const uint32_t DECOMP_SEQ[] = {")
for i in range(0, len(seq), 12):
    w("    " + ", ".join("0x%X" % v for v in seq[i:i + 12]) + ",")
w("};")
w("static const uint32_t DECOMP[] = {   // cp, compat<<7 | len, offset")
for cp, fl, off in dent:
    w("    0x%X, %d, %d," % (cp, fl, off))
w("};")
w("const struct udecomp uni_decomp = { DECOMP, %d, DECOMP_SEQ };" % len(dent))
ccc_items = sorted(ccc.items())
w("static const uint32_t CCC[] = {")
flat = []
for a, b in ccc_items:
    flat += [a, b]
for i in range(0, len(flat), 12):
    w("    " + ", ".join("0x%X" % v for v in flat[i:i + 12]) + ",")
w("};")
w("const struct upairs uni_ccc = { CCC, %d };" % len(ccc_items))
w("static const uint32_t COMPOSE[] = {   // first, second, composite")
for a, b, c in compose:
    w("    0x%X, 0x%X, 0x%X," % (a, b, c))
w("};")
w("const struct ucompose uni_compose = { COMPOSE, %d };" % len(compose))
print("\n".join(out))
