#include "wdom.h"
#include "wcommon.h"
#include "charsets.h"
#include "charsets_mb.h"

// Byte stream -> UTF-8 with newline normalization (CRLF / CR -> LF), as the
// HTML input stream preprocessing requires. Supported: UTF-8 (validated,
// errors -> U+FFFD), UTF-16LE/BE (BOM or label), the single-byte code pages in
// charsets.h, and the multibyte CJK encodings in charsets_mb.h (GBK/gb18030,
// Shift_JIS, Big5; WHATWG decoders). Unknown labels decode as UTF-8 and fall
// back to windows-1252 when the bytes are not valid UTF-8 (legacy pages).

static int label_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (w_lower((unsigned char)*a) != w_lower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int find_cs(const char* label) {
    if (!label || !label[0]) return -1;
    for (int i = 0; i < WCS_COUNT; i++)
        for (const char* const* l = wcs_table[i].labels; *l; l++)
            if (label_eq(label, *l)) return i;
    for (int i = 0; i < WMB_COUNT; i++)
        for (const char* const* l = wmbcs_table[i].labels; *l; l++)
            if (label_eq(label, *l)) return WCS_COUNT + i;
    return -1;
}

// last gb18030 range whose start pointer is <= p (the WHATWG four-byte lookup)
static uint32_t gb_range_cp(uint32_t p) {
    if (p > 39419 && p < 189000) return 0xFFFD;
    if (p > 1237575) return 0xFFFD;
    if (p == 7457) return 0xE7C7;
    int lo = 0, hi = (int)(sizeof(wmb_gb_ranges) / sizeof(wmb_gb_ranges[0])) - 1;
    while (lo < hi) {
        int m = (lo + hi + 1) / 2;
        if (wmb_gb_ranges[m][0] <= p) lo = m; else hi = m - 1;
    }
    uint32_t cp = wmb_gb_ranges[lo][1] + (p - wmb_gb_ranges[lo][0]);
    return cp <= 0x10FFFF ? cp : 0xFFFD;
}

// decode one multibyte two/four-byte sequence at u[i] (len bytes total); the
// caller has handled ASCII and the encoding's single-byte specials
static uint32_t mb_seq(const unsigned char* u, int len, int i, int type, int* consumed) {
    *consumed = 1;
    unsigned c = u[i];
    if (c < 0x81 || c > 0xFE || i + 1 >= len) return 0xFFFD;
    unsigned t = u[i + 1];
    if (type == WMB_GB18030 && t >= 0x30 && t <= 0x39) {
        // four-byte form
        if (i + 3 >= len) return 0xFFFD;
        unsigned b3 = u[i + 2], b4 = u[i + 3];
        if (b3 < 0x81 || b3 > 0xFE || b4 < 0x30 || b4 > 0x39) { *consumed = 2; return 0xFFFD; }
        uint32_t p = ((uint32_t)(c - 0x81) * 10 + (t - 0x30)) * 1260 + (uint32_t)(b3 - 0x81) * 10 + (b4 - 0x30);
        *consumed = 4;
        return gb_range_cp(p);
    }
    int ptr = -1;
    if (type == WMB_GBK || type == WMB_GB18030) {
        if (t >= 0x40 && t <= 0xFE && t != 0x7F) {
            int p = (c - 0x81) * 190 + (t < 0x7F ? t - 0x40 : t - 0x41);
            if (p < (int)(sizeof(wmb_gb18030) / sizeof(wmb_gb18030[0]))) ptr = p;
        }
    } else if (type == WMB_SJIS) {
        if (((t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFC)) &&
            ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC))) {
            int p = (c - 0x81) * 188 + (t < 0x7F ? t - 0x40 : t - 0x41);
            if (p < (int)(sizeof(wmb_jis0208) / sizeof(wmb_jis0208[0]))) ptr = p;
        }
    } else {   // WMB_BIG5
        if ((t >= 0x40 && t <= 0x7E) || (t >= 0xA1 && t <= 0xFE)) {
            int p = (c - 0x81) * 157 + (t < 0x7F ? t - 0x40 : t - 0x62);
            if (p < (int)(sizeof(wmb_big5) / sizeof(wmb_big5[0]))) ptr = p;
        }
    }
    *consumed = 2;
    if (ptr < 0) return 0xFFFD;
    uint32_t cp = type == WMB_SJIS ? wmb_jis0208[ptr] : type == WMB_BIG5 ? wmb_big5[ptr] : wmb_gb18030[ptr];
    return cp == 0xFFFF ? 0xFFFD : cp;
}

static int valid_utf8(const char* s, int n) {
    for (int i = 0; i < n;) {
        unsigned c = (unsigned char)s[i];
        if (c < 0x80) { i++; continue; }
        uint32_t cp;
        int l = w_utf8_dec(s + i, n - i, &cp);
        if (cp == 0xFFFD && !(l == 3 && (unsigned char)s[i] == 0xEF)) {
            if (n - i < 4) return 1; // truncated tail: tolerate
            return 0;
        }
        i += l;
    }
    return 1;
}

static void put_cp(struct wbuf* b, uint32_t cp, int* last_cr) {
    if (cp == '\r') { wbuf_putc(b, '\n'); *last_cr = 1; return; }
    if (cp == '\n' && *last_cr) { *last_cr = 0; return; }
    *last_cr = 0;
    char u[4];
    int l = w_utf8_enc(cp, u);
    wbuf_put(b, u, l);
}

char* wcharset_decode(const char* in, int len, const char* label, int* out_len,
                      char* used, int used_cap) {
    struct wbuf b = { 0, 0, 0 };
    wbuf_reserve(&b, len + len / 8 + 16);
    int last_cr = 0;
    const char* name = "utf-8";
    int mode = 0; // 0 utf8, 1 utf16le, 2 utf16be, 3 single-byte, 4 multibyte CJK
    int cs = -1;
    int start = 0;
    const unsigned char* u = (const unsigned char*)in;
    if (len >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) { start = 3; }
    else if (len >= 2 && u[0] == 0xFF && u[1] == 0xFE) { mode = 1; start = 2; name = "utf-16le"; }
    else if (len >= 2 && u[0] == 0xFE && u[1] == 0xFF) { mode = 2; start = 2; name = "utf-16be"; }
    else if (label && (label_eq(label, "utf-16le") || label_eq(label, "utf-16"))) { mode = 1; name = "utf-16le"; }
    else if (label && label_eq(label, "utf-16be")) { mode = 2; name = "utf-16be"; }
    else if ((cs = find_cs(label)) >= 0) {
        if (cs >= WCS_COUNT) { mode = 4; name = wmbcs_table[cs - WCS_COUNT].labels[0]; }
        else {
            mode = 3; name = wcs_table[cs].labels[0];
            // A page labelled latin-1 but actually UTF-8 is common; trust bytes.
            if (cs == 0 && valid_utf8(in, len)) {
                int hi = 0;
                for (int i = 0; i < len && !hi; i++) if (u[i] >= 0x80) hi = 1;
                if (hi) { mode = 0; name = "utf-8"; }
            }
        }
    } else if (!valid_utf8(in, len)) {
        mode = 3; cs = 0; name = "windows-1252";
    }
    if (mode == 0) {
        for (int i = start; i < len;) {
            unsigned c = u[i];
            if (c < 0x80) {
                if (c == '\r') { wbuf_putc(&b, '\n'); last_cr = 1; i++; continue; }
                if (c == '\n' && last_cr) { last_cr = 0; i++; continue; }
                last_cr = 0;
                // fast path for ASCII runs
                int j = i;
                while (j < len && u[j] < 0x80 && u[j] != '\r') j++;
                wbuf_put(&b, in + i, j - i);
                if (j > i && in[j - 1] == '\n') last_cr = 0;
                i = j;
                continue;
            }
            uint32_t cp;
            int l = w_utf8_dec(in + i, len - i, &cp);
            put_cp(&b, cp, &last_cr);
            i += l;
        }
    } else if (mode == 1 || mode == 2) {
        for (int i = start; i + 1 < len; i += 2) {
            uint32_t cp = mode == 1 ? (u[i] | (u[i + 1] << 8)) : ((u[i] << 8) | u[i + 1]);
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len) {
                uint32_t lo = mode == 1 ? (u[i + 2] | (u[i + 3] << 8)) : ((u[i + 2] << 8) | u[i + 3]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            put_cp(&b, cp, &last_cr);
        }
    } else if (mode == 3) {
        const uint16_t* hi = wcs_table[cs].hi;
        for (int i = start; i < len; i++) {
            unsigned c = u[i];
            put_cp(&b, c < 0x80 ? c : hi[c - 0x80], &last_cr);
        }
    } else {
        int type = wmbcs_table[cs - WCS_COUNT].type;
        for (int i = start; i < len;) {
            unsigned c = u[i];
            if (c < 0x80) { put_cp(&b, c, &last_cr); i++; continue; }
            int consumed = 1;
            uint32_t cp;
            if (type == WMB_SJIS && c >= 0xA1 && c <= 0xDF) cp = 0xFF61 + c - 0xA1;
            else if (type == WMB_SJIS && c == 0x80) cp = 0x80;
            else if ((type == WMB_GBK || type == WMB_GB18030) && c == 0x80) cp = 0x20AC;
            else cp = mb_seq(u, len, i, type, &consumed);
            put_cp(&b, cp, &last_cr);
            i += consumed;
        }
    }
    if (used && used_cap > 0) {
        int k = 0;
        while (name[k] && k < used_cap - 1) { used[k] = name[k]; k++; }
        used[k] = 0;
    }
    wbuf_putc(&b, 0);
    *out_len = b.len - 1;
    return b.p;
}

// Prescan for <meta charset=...> or <meta http-equiv=content-type
// content="...charset=..."> in the first 4KB (WHATWG prescan, simplified).
int wcharset_sniff(const char* in, int len, char* out, int cap) {
    if (len > 4096) len = 4096;
    out[0] = 0;
    for (int i = 0; i + 5 < len; i++) {
        if (in[i] != '<') continue;
        if (in[i + 1] == '!' && in[i + 2] == '-' && in[i + 3] == '-') {
            int j = i + 4;
            while (j + 2 < len && !(in[j] == '-' && in[j + 1] == '-' && in[j + 2] == '>')) j++;
            i = j;
            continue;
        }
        if (!w_ieq_prefix(in + i + 1, len - i - 1, "meta")) continue;
        int j = i + 5;
        int end = j;
        while (end < len && in[end] != '>') end++;
        // look for "charset" anywhere inside the tag
        for (int k = j; k + 7 < end; k++) {
            if (!w_ieq_prefix(in + k, end - k, "charset")) continue;
            int p = k + 7;
            while (p < end && w_isspace((unsigned char)in[p])) p++;
            if (p >= end || in[p] != '=') continue;
            p++;
            while (p < end && (w_isspace((unsigned char)in[p]) || in[p] == '"' || in[p] == '\'')) p++;
            int o = 0;
            while (p < end && o < cap - 1 && !w_isspace((unsigned char)in[p]) &&
                   in[p] != '"' && in[p] != '\'' && in[p] != ';' && in[p] != '/' && in[p] != '>')
                out[o++] = in[p++];
            out[o] = 0;
            if (o) return 1;
        }
        i = end;
    }
    return 0;
}
