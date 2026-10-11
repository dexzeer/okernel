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
