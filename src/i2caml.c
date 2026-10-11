// I2C-HID device lookup in AML (see i2caml.h).
#include "i2caml.h"

static int find(const uint8_t* t, uint32_t from, uint32_t end, const void* pat, uint32_t pl) {
    const uint8_t* p = (const uint8_t*)pat;
    for (uint32_t i = from; i + pl <= end; i++) {
        if (t[i] != p[0]) continue;
        uint32_t k = 1;
        while (k < pl && t[i + k] == p[k]) k++;
        if (k == pl) return (int)i;
    }
    return -1;
}

static int known_id(const char* s) {
    static const char* const ids[] = { "SYNA", "ELAN", "ALPS", "CYAP", "MSFT0001", "PNP0C50", "ACPI0C50", "FTE", "GXTP", "ATML", "WCOM", "HPQ", 0 };
    for (int i = 0; ids[i]; i++) {
        int k = 0;
        while (ids[i][k] && s[k] == ids[i][k]) k++;
        if (!ids[i][k]) return 1;
    }
    return 0;
}

// the string following the 4-character name `nm` (a Name(_HID, "...")), or 0
static int name_string(const uint8_t* t, uint32_t from, uint32_t end, const char* nm, char* out, int cap) {
    int p = find(t, from, end, nm, 4);
    if (p < 0 || (uint32_t)p + 5 >= end) return 0;
    if (t[p + 4] == 0x0D) {
        int k = 0;
        while (k < cap - 1 && (uint32_t)(p + 5 + k) < end && t[p + 5 + k] >= 0x20 && t[p + 5 + k] < 0x7F) { out[k] = (char)t[p + 5 + k]; k++; }
        out[k] = 0;
        return k > 0;
    }
    if (t[p + 4] == 0x0C && (uint32_t)p + 9 <= end) {            // EISAID dword: PNP0C50 = 0x500CD041
        uint32_t v = (uint32_t)t[p + 5] | ((uint32_t)t[p + 6] << 8) | ((uint32_t)t[p + 7] << 16) | ((uint32_t)t[p + 8] << 24);
        const char* s = v == 0x500CD041u ? "PNP0C50" : 0;
        if (!s) return 0;
        int k = 0;
        while (s[k] && k < cap - 1) { out[k] = s[k]; k++; }
        out[k] = 0;
        return 1;
    }
    return 0;
}

static void add_reg(struct i2c_hid_dev* d, uint16_t v) {
    for (int i = 0; i < d->nreg; i++) if (d->reg[i] == v) return;
    if (d->nreg < 8) d->reg[d->nreg++] = v;
}

// ---- where the firmware put the controller's registers ----------------------------------------------
// Intel LPSS devices in ACPI mode have no PCI function; their _CRS fills the Memory32Fixed base from a named field:
//   CreateDWordField(RBUF, 4, B0BA) ... Store(I50A, B0BA)
// where I50A is a field of a SystemMemory OperationRegion (the firmware's GNVS block in ACPI NVS memory), and _PS0
// powers the device by clearing the D-state bits of a PCI-config mirror: OperationRegion(KEYS, SystemMemory, I51A, 0x100)
// with PSAT at +0x84. We find those names by pattern, resolve each to a physical address, and read it.
static uint32_t pkg(const uint8_t* t, uint32_t i, uint32_t* hdr) {
    uint32_t lead = t[i], cnt = (lead >> 6) & 3;
    uint32_t v = cnt == 0 ? (lead & 0x3F) : (lead & 0x0F);
    for (uint32_t k = 0; k < cnt; k++) v |= (uint32_t)t[i + 1 + k] << (4 + 8 * k);
    *hdr = 1 + cnt;
    return v;
}

// constant integer at t[i]: ByteConst/WordConst/DWordConst/Zero/One. Returns bytes used or 0.
static int aml_int(const uint8_t* t, uint32_t i, uint32_t end, uint32_t* v) {
    if (i >= end) return 0;
    if (t[i] == 0x00) { *v = 0; return 1; }
    if (t[i] == 0x01) { *v = 1; return 1; }
    if (t[i] == 0x0A && i + 2 <= end) { *v = t[i + 1]; return 2; }
    if (t[i] == 0x0B && i + 3 <= end) { *v = (uint32_t)t[i + 1] | ((uint32_t)t[i + 2] << 8); return 3; }
    if (t[i] == 0x0C && i + 5 <= end) { *v = (uint32_t)t[i + 1] | ((uint32_t)t[i + 2] << 8) | ((uint32_t)t[i + 3] << 16) | ((uint32_t)t[i + 4] << 24); return 5; }
    return 0;
}

// physical address of the SystemMemory field `nm` (a 32 bit field at a byte boundary), searching every table
static int field_phys(const uint8_t* const* tabs, const uint32_t* lens, int nt, const char* nm, uint32_t* phys) {
    for (int ti = 0; ti < nt; ti++) {
        const uint8_t* t = tabs[ti];
        uint32_t len = lens[ti];
        for (uint32_t i = 36; i + 8 < len; i++) {
            if (t[i] != 0x5B || t[i + 1] != 0x81) continue;                          // FieldOp
            uint32_t hdr, pl = pkg(t, i + 2, &hdr);
            uint32_t end = i + 2 + pl, j = i + 2 + hdr;
            if (end > len || j + 5 > end) continue;
            const uint8_t* rn = t + j;                                               // region name
            j += 5;                                                                  // + flags
            uint32_t bit = 0;
            int hit = 0;
            uint32_t fbit = 0;
            while (j < end && !hit) {
                uint8_t b = t[j];
                if (b == 0) { uint32_t h2, l = pkg(t, j + 1, &h2); bit += l; j += 1 + h2; }
                else if (b == 1) j += 3;
                else if (b == 2) j += 1;
                else if (b == 3) j += 4;
                else {
                    if (j + 5 > end) break;
                    uint32_t h2, l = pkg(t, j + 4, &h2);
                    if (t[j] == (uint8_t)nm[0] && t[j + 1] == (uint8_t)nm[1] && t[j + 2] == (uint8_t)nm[2] && t[j + 3] == (uint8_t)nm[3] && l == 32 && !(bit & 7)) { hit = 1; fbit = bit; }
                    bit += l; j += 4 + h2;
                }
            }
            if (!hit) continue;
            for (int tj = 0; tj < nt; tj++) {                                        // its OperationRegion(name, SystemMemory, const, len)
                const uint8_t* u = tabs[tj];
                uint32_t ul = lens[tj];
                for (uint32_t k = 36; k + 9 < ul; k++) {
                    if (u[k] != 0x5B || u[k + 1] != 0x80 || u[k + 2] != rn[0] || u[k + 3] != rn[1] || u[k + 4] != rn[2] || u[k + 5] != rn[3] || u[k + 6] != 0) continue;
                    uint32_t off;
                    if (!aml_int(u, k + 7, ul, &off) || !(u[k + 7] == 0x0C || u[k + 7] == 0x0B || u[k + 7] == 0x0A)) continue;
                    *phys = off + fbit / 8;
                    return 1;
                }
            }
        }
    }
    return 0;
}

// the Device() scope called `nm` in any table: its content range [*from, *to) in table *ti
static int device_range(const uint8_t* const* tabs, const uint32_t* lens, int nt, const char* nm, int* ti, uint32_t* from, uint32_t* to) {
    for (int k = 0; k < nt; k++) {
        const uint8_t* t = tabs[k];
        uint32_t len = lens[k];
        for (uint32_t i = 36; i + 12 < len; i++) {
            if (t[i] != 0x5B || t[i + 1] != 0x82) continue;
            uint32_t hdr, pl = pkg(t, i + 2, &hdr);
            uint32_t ns = i + 2 + hdr, end = i + 2 + pl;
            if (end > len || ns + 4 > end || t[ns] != (uint8_t)nm[0] || t[ns + 1] != (uint8_t)nm[1] || t[ns + 2] != (uint8_t)nm[2] || t[ns + 3] != (uint8_t)nm[3]) continue;
            *ti = k; *from = ns + 4; *to = end;
            return 1;
        }
    }
    return 0;
}

int i2caml_gpioint(const uint8_t* const* tabs, const uint32_t* lens, int nt, const char* devname, uint32_t* pin, char src[5]) {
    int ti; uint32_t from, to;
    if (!device_range(tabs, lens, nt, devname, &ti, &from, &to)) return 0;
    const uint8_t* t = tabs[ti];
    for (uint32_t j = from; j + 20 <= to; j++) {
        if (t[j] != 0x8C || t[j + 3] != 0x01 || t[j + 4] != 0x00) continue;                  // GpioInt: rev 1, type 0 = interrupt
        uint32_t dl = (uint32_t)t[j + 1] | ((uint32_t)t[j + 2] << 8);
        uint32_t po = (uint32_t)t[j + 14] | ((uint32_t)t[j + 15] << 8), so = (uint32_t)t[j + 17] | ((uint32_t)t[j + 18] << 8);
        if (dl < 18 || j + 3 + dl > to || po + 2 > dl + 3 || so >= dl + 3) continue;
        *pin = (uint32_t)t[j + po] | ((uint32_t)t[j + po + 1] << 8);
        uint32_t e = j + so;                                                              // the source path, NUL terminated: keep its last 4 characters
        uint32_t k = 0;
        while (e + k < j + 3 + dl && t[e + k]) k++;
        for (int c = 0; c < 4; c++) src[c] = k >= 4 ? (char)t[e + k - 4 + (uint32_t)c] : '?';
        src[4] = 0;
        return 1;
    }
    return 0;
}

int i2caml_mem32(const uint8_t* const* tabs, const uint32_t* lens, int nt, const char* devname, uint32_t* base, int* is_chv) {
    int ti; uint32_t from, to;
    if (!device_range(tabs, lens, nt, devname, &ti, &from, &to)) return 0;
    const uint8_t* t = tabs[ti];
    *is_chv = 0;
    for (uint32_t j = from; j + 7 <= to; j++)
        if (t[j] == 'I' && t[j + 1] == 'N' && t[j + 2] == 'T' && t[j + 3] == '3' && t[j + 4] == '3' && t[j + 5] == 'F' && t[j + 6] == 'F') *is_chv = 1;
    for (uint32_t j = from; j + 12 <= to; j++) {
        if (t[j] != 0x86 || t[j + 1] != 0x09 || t[j + 2] != 0x00) continue;               // Memory32Fixed
        *base = (uint32_t)t[j + 4] | ((uint32_t)t[j + 5] << 8) | ((uint32_t)t[j + 6] << 16) | ((uint32_t)t[j + 7] << 24);
        return 1;
    }
    return 0;
}

int i2caml_ctrl_bars(const uint8_t* const* tabs, const uint32_t* lens, int nt, const char* ctrl, i2caml_rd rd, struct i2c_ctrl_bars* o) {
    o->ok = 0; o->bar0 = o->bar1 = o->len0 = 0; o->psat_known = 0;
    int cl = 0;
    while (ctrl[cl]) cl++;
    if (cl < 4) return 0;
    const char* nm = ctrl + cl - 4;
    for (int ti = 0; ti < nt; ti++) {
        const uint8_t* t = tabs[ti];
        uint32_t len = lens[ti];
        for (uint32_t i = 36; i + 12 < len; i++) {
            if (t[i] != 0x5B || t[i + 1] != 0x82) continue;
            uint32_t hdr, pl = pkg(t, i + 2, &hdr);
            uint32_t ns = i + 2 + hdr, end = i + 2 + pl;
            if (end > len || ns + 4 > end || t[ns] != (uint8_t)nm[0] || t[ns + 1] != (uint8_t)nm[1] || t[ns + 2] != (uint8_t)nm[2] || t[ns + 3] != (uint8_t)nm[3]) continue;
            char n0[4] = { 0 }, n1[4] = { 0 }, nl[4] = { 0 };
            int h0 = 0, h1 = 0, hl = 0;
            for (uint32_t j = ns + 4; j + 9 <= end; j++) {
                if (t[j] == 0x70 && t[j + 5] == 'B' && t[j + 6] == '0' && t[j + 7] == 'B' && t[j + 8] == 'A') { for (int k = 0; k < 4; k++) n0[k] = (char)t[j + 1 + k]; h0 = 1; }   // Store(I50A, B0BA)
                if (t[j] == 0x70 && t[j + 5] == 'B' && t[j + 6] == '0' && t[j + 7] == 'L' && t[j + 8] == 'N') { for (int k = 0; k < 4; k++) nl[k] = (char)t[j + 1 + k]; hl = 1; }   // Store(I50L, B0LN)
                if (j + 11 <= end && t[j] == 0x5B && t[j + 1] == 0x80 && t[j + 2] == 'K' && t[j + 3] == 'E' && t[j + 4] == 'Y' && t[j + 5] == 'S' && t[j + 6] == 0x00 &&
                    ((t[j + 7] >= 'A' && t[j + 7] <= 'Z') || t[j + 7] == '_')) { for (int k = 0; k < 4; k++) n1[k] = (char)t[j + 7 + k]; h1 = 1; }   // OperationRegion(KEYS, SystemMemory, I51A, ..)
            }
            uint32_t pa;
            if (h0 && field_phys(tabs, lens, nt, n0, &pa) && rd(pa, &o->bar0)) { o->ok = 1; }
            if (hl && field_phys(tabs, lens, nt, nl, &pa)) rd(pa, &o->len0);
            if (h1 && field_phys(tabs, lens, nt, n1, &pa) && rd(pa, &o->bar1)) o->psat_known = 1;
            return o->ok;
        }
    }
    return 0;
}

int i2caml_find(const uint8_t* t, uint32_t len, struct i2c_hid_dev* out, int max) {
    int n = 0;
    static const uint8_t guid[16] = { 0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45, 0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE };
    for (uint32_t i = 36; i + 12 < len && n < max; i++) {
        if (t[i] != 0x5B || t[i + 1] != 0x82) continue;                           // DeviceOp
        uint32_t p = i + 2, lead = t[p], cnt = (lead >> 6) & 3;
        uint32_t pk = cnt == 0 ? (lead & 0x3F) : (lead & 0x0F);
        for (uint32_t k = 0; k < cnt; k++) pk |= (uint32_t)t[p + 1 + k] << (4 + 8 * k);
        uint32_t nstart = p + 1 + cnt;
        uint32_t end = p + pk;
        if (pk < 8 || end > len) continue;
        struct i2c_hid_dev d;
        uint8_t* z = (uint8_t*)&d;
        for (unsigned k = 0; k < sizeof d; k++) z[k] = 0;
        char cid[12] = "";
        int has_hid = name_string(t, nstart + 4, end, "_HID", d.hid, sizeof d.hid);
        int has_cid = name_string(t, nstart + 4, end, "_CID", cid, sizeof cid);
        if (!((has_hid && known_id(d.hid)) || (has_cid && known_id(cid)))) continue;
        // I2cSerialBusV2: 8E len(2) rev=1 srcidx type=1 flags tsflags(2) tsrev=1 tdlen(2)>=6 speed(4) addr(2) [vendor] source\0
        int found = 0;
        for (uint32_t j = nstart + 4; j + 20 < end; j++) {
            const uint8_t* q = t + j;
            if (q[0] != 0x8E || q[3] != 0x01 || q[5] != 0x01 || q[9] != 0x01 || q[11] != 0x00 || q[10] < 6) continue;
            d.speed = (uint32_t)q[12] | ((uint32_t)q[13] << 8) | ((uint32_t)q[14] << 16) | ((uint32_t)q[15] << 24);
            d.addr = (q[16] | (q[17] << 8)) & 0x3FF;
            uint32_t sp = 12 + q[10];
            int k = 0;
            while (k < (int)sizeof d.ctrl - 1 && j + sp + k < end && q[sp + k] >= 0x20 && q[sp + k] < 0x7F) { d.ctrl[k] = (char)q[sp + k]; k++; }
            d.ctrl[k] = 0;
            found = 1;
            break;
        }
        if (!found) continue;
        if (!d.hid[0]) { for (int k = 0; k < 11 && cid[k]; k++) d.hid[k] = cid[k]; }
        for (int k = 0; k < 4; k++) d.name[k] = (char)t[nstart + (uint32_t)k];
        d.name[4] = 0;
        int g = find(t, nstart + 4, end, guid, 16);
        if (g >= 0) {
            uint32_t stop = (uint32_t)g + 16 + 500;
            if (stop > end) stop = end;
            for (uint32_t j = (uint32_t)g + 16; j + 2 <= stop; j++) {
                if (t[j] != 0xA4) continue;                                       // ReturnOp
                if (t[j + 1] == 0x0A && j + 3 <= stop) add_reg(&d, t[j + 2]);     // ByteConst
                else if (t[j + 1] == 0x0B && j + 4 <= stop) add_reg(&d, (uint16_t)(t[j + 2] | (t[j + 3] << 8)));   // WordConst
                else if (t[j + 1] == 0x01) add_reg(&d, 1);                        // One
                else if (t[j + 1] == 0x00) add_reg(&d, 0);                        // Zero
            }
        }
        out[n++] = d;
        i = nstart;                                                               // inner devices are scanned too
    }
    return n;
}
