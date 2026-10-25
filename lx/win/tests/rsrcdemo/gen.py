"""Writes the binary resources of rsrcdemo (run by lx/win/build.sh): a bitmap, an icon, a menu, a dialog, accelerators, strings."""
import struct

def w16(v): return struct.pack("<H", v)
def w32(v): return struct.pack("<I", v)
def wstr(s): return (s + "\0").encode("utf-16le")

# test.bmp: 16x16, 24 bit, left half red, right half blue
rows = b""
for y in range(16):
    row = b"".join((b"\x00\x00\xff" if x < 8 else b"\xff\x00\x00") for x in range(16))
    rows += row + b"\0" * ((4 - len(row) % 4) % 4)
hdr = struct.pack("<IiiHHIIiiII", 40, 16, 16, 1, 24, 0, len(rows), 0, 0, 0, 0)
bmp = b"BM" + w32(14 + len(hdr) + len(rows)) + w32(0) + w32(14 + len(hdr)) + hdr + rows
open("test.bmp", "wb").write(bmp)

# test.ico: a 16x16 32 bit image with alpha (green, transparent top-left corner) and a 32x32 8 bit image (palette: red) with an AND mask
def ico_image_32(size):
    pix = b""
    for y in range(size - 1, -1, -1):
        for x in range(size):
            a = 0 if (x < 2 and y < 2) else 255
            pix += bytes([0, 200, 0, a])
    mask = b"\0" * (((size + 31) // 32) * 4 * size)
    return struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(pix) + len(mask), 0, 0, 0, 0) + pix + mask
def ico_image_8(size):
    pal = b"".join(bytes([0, 0, 255, 0]) if i == 1 else bytes([0, 0, 0, 0]) for i in range(256))
    rows = b""
    for y in range(size):
        row = bytes([1] * size)
        rows += row + b"\0" * ((4 - len(row) % 4) % 4)
    mask = b""
    for y in range(size - 1, -1, -1):
        bits = 0xFFFFFFFF if y == 0 else 0                          # the (bottom-up) last row = the top row is transparent
        mask += struct.pack(">I", bits)
    return struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 8, 0, len(rows) + len(mask), 0, 0, 256, 0) + pal + rows + mask
i16, i32 = ico_image_32(16), ico_image_8(32)
off = 6 + 16 * 2
ico = struct.pack("<HHH", 0, 1, 2)
ico += struct.pack("<BBBBHHII", 16, 16, 0, 0, 1, 32, len(i16), off)
ico += struct.pack("<BBBBHHII", 32, 32, 0, 0, 1, 8, len(i32), off + len(i16))
ico += i16 + i32
open("test.ico", "wb").write(ico)

# menu.bin (old format): File { New 101, Exit 103 }, Help { About 301 }
def menu_item(opt, id_, text): return w16(opt) + w16(id_) + wstr(text)
def popup(opt, text): return w16(opt | 0x10) + wstr(text)
menu = w16(0) + w16(0)
menu += popup(0, "&File") + menu_item(0, 101, "&New\tCtrl+N") + menu_item(0x80, 103, "E&xit")
menu += popup(0x80, "&Help") + menu_item(0x80, 301, "&About")
open("menu.bin", "wb").write(menu)

# dialog.bin (old format): a static, an edit (101) with a text, OK, Cancel
def align4(b): return b + b"\0" * ((4 - len(b) % 4) % 4)
def item(style, x, y, cx, cy, id_, cls, text):
    return align4(w32(style | 0x50000000) + w32(0) + w16(x) + w16(y) + w16(cx) + w16(cy) + w16(id_) + w16(0xFFFF) + w16(cls) + wstr(text) + w16(0))
dlg = w32(0x90C80080) + w32(0) + w16(4) + w16(0) + w16(0) + w16(150) + w16(70) + w16(0) + w16(0) + wstr("Resource dialog")
dlg = align4(dlg)
dlg += item(0, 8, 8, 40, 10, 0xFFFF, 0x82, "Name:")
dlg += item(0x10000, 50, 6, 90, 14, 101, 0x81, "from resource")
dlg += item(0x10000 | 1, 20, 48, 40, 14, 1, 0x80, "OK")
dlg += item(0x10000, 80, 48, 40, 14, 2, 0x80, "Cancel")
open("dialog.bin", "wb").write(dlg)

# accel.bin: Ctrl+N -> 101 (FVIRTKEY | FCONTROL | last)
open("accel.bin", "wb").write(w16(0x80 | 1 | 8) + w16(ord("N")) + w16(101) + w16(0))

open("strings.txt", "w", encoding="utf-8").write("1 Hello string\n17 Second block\n")
open("data.bin", "wb").write(b"RCDATA-OK")
