"""Windows x64 unwind tables for the code gcc compiled for the PE side.

gcc describes every function's stack frame as DWARF call frame information (.eh_frame). Windows wants the same as RUNTIME_FUNCTION
entries (.pdata) pointing at UNWIND_INFO (.xdata): a list of prologue operations (push reg, allocate n bytes, save reg at offset...).
This module reads the CFI and writes those tables. A function whose frame cannot be described that way (a frame pointer with
later allocations, odd save slots) gets no entry and is reported on stderr: unwinding through it would be wrong.

    build(eh_frame_bytes, eh_frame_addr, base) -> (list of (begin_rva, end_rva, xdata_offset), xdata_bytes, unsupported_names)
"""
import struct, sys

# DWARF register number -> Windows unwind register number (rax rcx rdx rbx rsp rbp rsi rdi r8..r15)
DW2WIN = {0: 0, 1: 2, 2: 1, 3: 3, 4: 6, 5: 7, 6: 5, 7: 4}
for _r in range(8, 16): DW2WIN[_r] = _r
CALLEE_SAVED_WIN = {3, 5, 6, 7, 12, 13, 14, 15}
DW_RSP, DW_RBP = 7, 6

UWOP_PUSH_NONVOL, UWOP_ALLOC_LARGE, UWOP_ALLOC_SMALL, UWOP_SET_FPREG = 0, 1, 2, 3
UWOP_SAVE_NONVOL, UWOP_SAVE_NONVOL_FAR, UWOP_SAVE_XMM128, UWOP_SAVE_XMM128_FAR = 4, 5, 8, 9


def uleb(d, p):
    r = 0; sh = 0
    while True:
        b = d[p]; p += 1
        r |= (b & 0x7F) << sh; sh += 7
        if not b & 0x80: return r, p

def sleb(d, p):
    r = 0; sh = 0
    while True:
        b = d[p]; p += 1
        r |= (b & 0x7F) << sh; sh += 7
        if not b & 0x80:
            if b & 0x40: r -= 1 << sh
            return r, p


class State:
    def __init__(self):
        self.cfa_reg = DW_RSP; self.cfa_off = 8; self.saved = {}
    def copy(self):
        s = State(); s.cfa_reg = self.cfa_reg; s.cfa_off = self.cfa_off; s.saved = dict(self.saved); return s


def run_cfi(ins, cie, st, loc, events, stop_at_remember):
    """Interpret CFA instructions. events: list of (loc, kind, ...) in program order. Returns (state, loc, body) where body is
    the state at the first remember_state (epilogue start of gcc's code) or None."""
    p = 0; stack = []; init = cie["init_state"] if cie.get("init_state") else st.copy()
    body = None
    snaps = []
    while p < len(ins):
        op = ins[p]; p += 1
        hi, lo = op & 0xC0, op & 0x3F
        if hi == 0x40: snaps.append((loc, st.copy())); loc += lo * cie["caf"]
        elif hi == 0x80:
            off, p = uleb(ins, p); off *= cie["daf"]; st.saved[lo] = off; events.append((loc, "save", lo, off))
        elif hi == 0xC0:
            st.saved.pop(lo, None)
        elif op == 0x00: pass
        elif op == 0x02: snaps.append((loc, st.copy())); loc += ins[p] * cie["caf"]; p += 1
        elif op == 0x03: snaps.append((loc, st.copy())); loc += struct.unpack_from("<H", ins, p)[0] * cie["caf"]; p += 2
        elif op == 0x04: snaps.append((loc, st.copy())); loc += struct.unpack_from("<I", ins, p)[0] * cie["caf"]; p += 4
        elif op == 0x05:
            r, p = uleb(ins, p); off, p = uleb(ins, p); off *= cie["daf"]; st.saved[r] = off; events.append((loc, "save", r, off))
        elif op == 0x11:
            r, p = uleb(ins, p); off, p = sleb(ins, p); off *= cie["daf"]; st.saved[r] = off; events.append((loc, "save", r, off))
        elif op == 0x06:
            r, p = uleb(ins, p); st.saved.pop(r, None)
        elif op in (0x07, 0x08):
            r, p = uleb(ins, p); st.saved.pop(r, None)
        elif op == 0x09:
            r, p = uleb(ins, p); r2, p = uleb(ins, p)
        elif op == 0x0A:
            if body is None: body = (loc, st.copy(), len(events))
            stack.append(st.copy())
        elif op == 0x0B:
            if stack: st = stack.pop()
        elif op == 0x0C:
            r, p = uleb(ins, p); off, p = uleb(ins, p)
            events.append((loc, "cfareg" if r != st.cfa_reg else "cfaoff", r, off, st.cfa_off)); st.cfa_reg = r; st.cfa_off = off
        elif op == 0x0D:
            r, p = uleb(ins, p); events.append((loc, "cfareg", r, st.cfa_off, st.cfa_off)); st.cfa_reg = r
        elif op == 0x0E:
            off, p = uleb(ins, p); events.append((loc, "cfaoff", st.cfa_reg, off, st.cfa_off)); st.cfa_off = off
        elif op == 0x12:
            r, p = uleb(ins, p); off, p = sleb(ins, p); st.cfa_reg = r; st.cfa_off = off * cie["daf"]
        elif op == 0x13:
            off, p = sleb(ins, p); events.append((loc, "cfaoff", st.cfa_reg, off * cie["daf"], st.cfa_off)); st.cfa_off = off * cie["daf"]
        elif op in (0x0F, 0x16):
            n, p = uleb(ins, p); p += n; raise ValueError("expression CFA")
        elif op in (0x10, 0x14, 0x15):
            r, p = uleb(ins, p)
            if op == 0x10: n, p = uleb(ins, p); p += n
            elif op == 0x14: _, p = uleb(ins, p)
            else: _, p = sleb(ins, p)
        elif op == 0x2E:
            _, p = uleb(ins, p)
        elif op == 0x2F:
            r, p = uleb(ins, p); off, p = uleb(ins, p)
        else:
            raise ValueError("unknown CFA op %#x" % op)
    snaps.append((loc, st.copy()))
    return st, loc, body, snaps


def parse_eh_frame(data, addr):
    """-> list of (pc_begin, pc_end, cie, instruction bytes)"""
    cies = {}; fdes = []; p = 0
    while p + 4 <= len(data):
        length = struct.unpack_from("<I", data, p)[0]
        if length == 0: break
        start = p + 4; end = start + length
        cid = struct.unpack_from("<I", data, start)[0]
        if cid == 0:
            q = start + 4
            ver = data[q]; q += 1
            e = data.index(b"\0", q); aug = data[q:e].decode(); q = e + 1
            caf, q = uleb(data, q); daf, q = sleb(data, q)
            if ver == 1: q += 1
            else: _, q = uleb(data, q)
            enc = 0
            if aug.startswith("z"):
                alen, q = uleb(data, q); aend = q + alen
                for ch in aug[1:]:
                    if ch == "R": enc = data[q]; q += 1
                    elif ch == "L": q += 1
                    elif ch == "P":
                        penc = data[q]; q += 1; q += 8 if (penc & 7) == 4 else 4
                q = aend
            cies[p] = dict(caf=caf, daf=daf, enc=enc, ins=data[q:end], aug=aug)
        else:
            cie = cies.get(start - cid)
            if cie is not None:
                q = start + 4
                fmt = cie["enc"] & 0x0F
                if fmt == 0x0B or fmt == 0x03: begin = struct.unpack_from("<i" if fmt == 0x0B else "<I", data, q)[0]; q += 4
                elif fmt == 0x0C or fmt == 0x04: begin = struct.unpack_from("<q", data, q)[0]; q += 8
                else: raise ValueError("FDE pointer encoding %#x" % cie["enc"])
                if cie["enc"] & 0x70 == 0x10: begin += addr + (q - 4 if fmt in (0x0B, 0x03) else q - 8)
                rng = struct.unpack_from("<I", data, q)[0] if fmt in (0x0B, 0x03) else struct.unpack_from("<Q", data, q)[0]
                q += 4 if fmt in (0x0B, 0x03) else 8
                if cie["aug"].startswith("z"):
                    alen, q = uleb(data, q); q += alen
                fdes.append((begin, begin + rng, cie, data[q:end]))
        p = end
    return fdes


def encode_codes(ops):
    """ops: list of (code_offset, op, info, extra_slots) in prologue order -> bytes in reverse order, slot count"""
    out = bytearray()
    for off, op, info, extra in reversed(ops):
        out += bytes([off, op | (info << 4)])
        for x in extra: out += struct.pack("<H", x)
    return bytes(out)


def function_info(fn_begin, fn_end, cie, ins):
    """-> UNWIND_INFO bytes, or None for a leaf (nothing to describe); raises ValueError for frames we cannot describe"""
    events = []
    st = State()
    run_cfi(cie["ins"], dict(caf=cie["caf"], daf=cie["daf"]), st, 0, [], False)       # initial rules (cfa = rsp+8)
    st0 = st.copy()
    st, loc_end, body, snaps = run_cfi(ins, dict(caf=cie["caf"], daf=cie["daf"]), st0.copy(), 0, events, True)
    if body is not None: body_loc, body_state, body_nev = body
    else:
        # no epilogue marker: the state with the deepest stack before the code returns
        def depth(s): return 1 << 30 if s[1].cfa_reg == DW_RBP else s[1].cfa_off
        best = max(snaps, key=depth)
        body_loc, body_state = best[0], best[1]
        body_nev = len(events)
    prolog = [e for e in events[:body_nev] if e[0] <= body_loc]
    # the events at one address belong together: a push is `cfaoff +8` and `save reg at -(new)`
    ops = []; used = set(); fp_set = False; max_off = 0
    cur_off = 8; cur_reg = DW_RSP
    for i, e in enumerate(prolog):
        if i in used: continue
        loc, kind = e[0], e[1]
        if loc > 255: raise ValueError("prologue longer than 255 bytes")
        if kind == "cfaoff" or (kind == "cfareg" and e[2] == DW_RSP):
            if cur_reg != DW_RSP: raise ValueError("stack change after a frame pointer")
            new = e[3]; delta = new - cur_off; cur_off = new
            if delta < 0: raise ValueError("stack shrinks in the prologue")
            pushed = None
            for j, f in enumerate(prolog):
                if j not in used and f[0] == loc and f[1] == "save" and f[3] == -new and delta == 8 and DW2WIN.get(f[2], -1) in CALLEE_SAVED_WIN:
                    pushed = (j, DW2WIN[f[2]]); break
            if pushed:
                used.add(pushed[0]); ops.append((loc, UWOP_PUSH_NONVOL, pushed[1], []))
            elif delta:
                if delta <= 128: ops.append((loc, UWOP_ALLOC_SMALL, delta // 8 - 1, []))
                elif delta < 512 * 1024: ops.append((loc, UWOP_ALLOC_LARGE, 0, [delta // 8]))
                else: ops.append((loc, UWOP_ALLOC_LARGE, 1, [delta & 0xFFFF, delta >> 16]))
                if delta % 8: raise ValueError("allocation not a multiple of 8")
            max_off = max(max_off, loc)
        elif kind == "cfareg":
            if e[2] != DW_RBP: raise ValueError("odd frame register")
            ops.append((loc, UWOP_SET_FPREG, 0, []))
            fp_set = True; cur_reg = DW_RBP; max_off = max(max_off, loc)
        elif kind == "save":
            r, off = e[2], e[3]
            if cur_reg != DW_RSP:
                # after `mov rbp, rsp` register saves cannot be placed (no way to know rsp), unless it is the push of rbp itself
                raise ValueError("register save after a frame pointer")
            o = cur_off + off                                        # from rsp at that point
            if o < 0: raise ValueError("save below rsp")
            if r in DW2WIN and DW2WIN[r] in CALLEE_SAVED_WIN:
                if o % 8: raise ValueError("unaligned save")
                if o // 8 < 65536: ops.append((loc, UWOP_SAVE_NONVOL, DW2WIN[r], [o // 8]))
                else: ops.append((loc, UWOP_SAVE_NONVOL_FAR, DW2WIN[r], [o & 0xFFFF, o >> 16]))
            elif 23 <= r <= 32:                                      # xmm6 .. xmm15
                if o % 16: raise ValueError("unaligned xmm save")
                if o // 16 < 65536: ops.append((loc, UWOP_SAVE_XMM128, r - 17, [o // 16]))
                else: ops.append((loc, UWOP_SAVE_XMM128_FAR, r - 17, [o & 0xFFFF, o >> 16]))
            elif r == 16: pass                                        # the return address
            max_off = max(max_off, loc)
    # a frame pointer function: after SET_FPREG the only thing Windows can describe is a constant offset: ours is 0
    if not ops: return None
    codes = encode_codes(ops)
    nslots = len(codes) // 2
    if nslots > 255: raise ValueError("too many unwind codes")
    if nslots & 1: codes += b"\0\0"
    frame = 5 if fp_set else 0
    hdr = bytes([1, max_off, nslots, frame])                          # version 1, no flags
    return hdr + codes


def build(data, addr, base, names=None):
    fdes = parse_eh_frame(data, addr)
    entries = []; xdata = bytearray(); bad = []
    for b, e, cie, ins in sorted(fdes, key=lambda x: x[0]):
        try:
            info = function_info(b, e, cie, ins)
        except ValueError as ex:
            bad.append((b, str(ex))); continue
        if info is None: continue
        while len(xdata) % 4: xdata.append(0)
        entries.append((b - base, e - base, len(xdata)))
        xdata += info
    return entries, bytes(xdata), bad
