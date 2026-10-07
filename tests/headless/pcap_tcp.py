#!/usr/bin/env python3
"""Per-connection TCP timing from a QEMU filter-dump pcap (guest 10.0.2.15):
SYN -> SYN-ACK, first client data -> first server data (the TLS/HTTP
request round trip), last byte, bytes each way, retransmissions.

    python3 tests/headless/pcap_tcp.py ~/okvm/<tag>.pcap
"""
import struct, sys

def packets(path):
    with open(path, "rb") as f:
        hdr = f.read(24)
        magic = struct.unpack("<I", hdr[:4])[0]
        en = "<" if magic in (0xa1b2c3d4, 0xa1b23c4d) else ">"
        nano = magic in (0xa1b23c4d, 0x4d3cb2a1)
        while True:
            h = f.read(16)
            if len(h) < 16: return
            ts, tf, incl, orig = struct.unpack(en + "IIII", h)
            data = f.read(incl)
            yield ts + tf / (1e9 if nano else 1e6), data

conns = {}
t_first = None
for t, d in packets(sys.argv[1]):
    if t_first is None: t_first = t
    if len(d) < 34 or d[12:14] != b"\x08\x00" or d[23] != 6: continue
    ihl = (d[14] & 15) * 4
    tot = struct.unpack(">H", d[16:18])[0]
    src, dst = d[26:30], d[30:34]
    tcp = d[14 + ihl:14 + tot]
    sp, dp, seq, ack = struct.unpack(">HHII", tcp[:12])
    off = (tcp[12] >> 4) * 4
    flags = tcp[13]
    plen = len(tcp) - off
    out = src == bytes([10, 0, 2, 15])
    key = (sp, ".".join(map(str, dst)), dp) if out else (dp, ".".join(map(str, src)), sp)
    c = conns.setdefault(key, dict(syn=None, synack=None, c1=None, s1=None, last=None, cb=0, sb=0,
                                   seqs=set(), rtx=0))
    if out and flags & 2: c["syn"] = c["syn"] or t
    if not out and flags & 0x12 == 0x12: c["synack"] = c["synack"] or t
    if plen:
        if out:
            c["cb"] += plen
            c["c1"] = c["c1"] or t
            if seq in c["seqs"]: c["rtx"] += 1
            c["seqs"].add(seq)
        else:
            c["sb"] += plen
            c["s1"] = c["s1"] or t
            c["last"] = t
ms = lambda a, b: "%6.0f" % ((b - a) * 1000) if a and b else "     -"
print("  start  port  server            syn->synack  req->resp1  resp1->last  out    in      rtx")
for k, c in sorted(conns.items(), key=lambda kv: kv[1]["syn"] or 0):
    st = (c["syn"] or 0) - t_first
    print("%7.2f %5d %-17s %s      %s      %s  %6d %7d %3d" % (st, k[0], k[1], ms(c["syn"], c["synack"]),
          ms(c["c1"], c["s1"]), ms(c["s1"], c["last"]), c["cb"], c["sb"], c["rtx"]))
