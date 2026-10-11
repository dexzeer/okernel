// WPA2-Personal cryptography; see wcrypto.h.
#include "wcrypto.h"
#include "../crypto/aes.h"
#include "../crypto/sha1.h"
#include "../string.h"

// ---- HMAC-SHA1 / PBKDF2 / PRF --------------------------------------------------------------------------------------------------
typedef struct { sha1_ctx in, out; } hmac1;

static void hmac1_init(hmac1* h, const uint8_t* key, uint32_t klen) {
    uint8_t k[64], pad[64];
    memset(k, 0, sizeof k);
    if (klen > 64) sha1(key, klen, k); else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha1_init(&h->in); sha1_update(&h->in, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha1_init(&h->out); sha1_update(&h->out, pad, 64);
}

static void hmac1_finish(const hmac1* h, sha1_ctx* in_state, uint8_t out[20]) {
    uint8_t inner[20];
    sha1_final(in_state, inner);
    sha1_ctx o = h->out;
    sha1_update(&o, inner, 20);
    sha1_final(&o, out);
}

void wc_hmac_sha1(const uint8_t* key, uint32_t klen, const uint8_t* msg, uint32_t mlen, uint8_t out[20]) {
    hmac1 h;
    hmac1_init(&h, key, klen);
    sha1_ctx i = h.in;
    sha1_update(&i, msg, mlen);
    hmac1_finish(&h, &i, out);
}

void wc_pbkdf2_sha1(const uint8_t* pass, uint32_t plen, const uint8_t* salt, uint32_t slen, uint32_t iters, uint8_t* out, uint32_t outlen) {
    hmac1 h;
    hmac1_init(&h, pass, plen);
    for (uint32_t blk = 1; outlen; blk++) {
        uint8_t u[20], t[20], be[4] = { (uint8_t)(blk >> 24), (uint8_t)(blk >> 16), (uint8_t)(blk >> 8), (uint8_t)blk };
        sha1_ctx i = h.in;
        sha1_update(&i, salt, slen);
        sha1_update(&i, be, 4);
        hmac1_finish(&h, &i, u);
        memcpy(t, u, 20);
        for (uint32_t n = 1; n < iters; n++) {
            i = h.in;
            sha1_update(&i, u, 20);
            hmac1_finish(&h, &i, u);
            for (int k = 0; k < 20; k++) t[k] ^= u[k];
        }
        uint32_t n = outlen < 20 ? outlen : 20;
        memcpy(out, t, n);
        out += n; outlen -= n;
    }
}

void wc_wpa_psk(const char* pass, const uint8_t* ssid, uint32_t ssid_len, uint8_t psk[32]) {
    wc_pbkdf2_sha1((const uint8_t*)pass, (uint32_t)strlen(pass), ssid, ssid_len, 4096, psk, 32);
}

void wc_prf_sha1(const uint8_t* key, uint32_t klen, const char* label, const uint8_t* data, uint32_t dlen, uint8_t* out, uint32_t outlen) {
    hmac1 h;
    hmac1_init(&h, key, klen);
    uint32_t ll = (uint32_t)strlen(label);
    for (uint32_t i = 0; outlen; i++) {
        uint8_t zero = 0, ctr = (uint8_t)i, d[20];
        sha1_ctx s = h.in;
        sha1_update(&s, (const uint8_t*)label, ll);
        sha1_update(&s, &zero, 1);
        sha1_update(&s, data, dlen);
        sha1_update(&s, &ctr, 1);
        hmac1_finish(&h, &s, d);
        uint32_t n = outlen < 20 ? outlen : 20;
        memcpy(out, d, n);
        out += n; outlen -= n;
    }
}

static int cmp(const uint8_t* a, const uint8_t* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

void wc_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48]) {
    uint8_t d[76];
    const uint8_t* lo = cmp(aa, spa, 6) < 0 ? aa : spa;
    const uint8_t* hi = lo == aa ? spa : aa;
    memcpy(d, lo, 6); memcpy(d + 6, hi, 6);
    const uint8_t* nlo = cmp(anonce, snonce, 32) < 0 ? anonce : snonce;
    const uint8_t* nhi = nlo == anonce ? snonce : anonce;
    memcpy(d + 12, nlo, 32); memcpy(d + 44, nhi, 32);
    wc_prf_sha1(pmk, 32, "Pairwise key expansion", d, 76, ptk, 48);
}

// ---- AES-128 decryption (own tables, own key schedule) -----------------------------------------------------------------------------
static uint8_t SB[256], ISB[256];
static int tables_ready;

static uint8_t rotl8(uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); }
static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static uint8_t gm(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
    return r;
}

static void init_tables(void) {
    if (tables_ready) return;
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));                 // p *= 3
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);  // q /= 3
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
        SB[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    SB[0] = 0x63;
    for (int i = 0; i < 256; i++) ISB[SB[i]] = (uint8_t)i;
    tables_ready = 1;
}

static void expand_key(const uint8_t key[16], uint8_t rk[176]) {
    memcpy(rk, key, 16);
    uint8_t rc = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
        if ((i & 15) == 0) {
            uint8_t a = t[0];
            t[0] = (uint8_t)(SB[t[1]] ^ rc); t[1] = SB[t[2]]; t[2] = SB[t[3]]; t[3] = SB[a];
            rc = xt(rc);
        }
        for (int k = 0; k < 4; k++) rk[i + k] = (uint8_t)(rk[i - 16 + k] ^ t[k]);
    }
}

void wc_aes128_decrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    init_tables();
    uint8_t rk[176], s[16], t[16];
    expand_key(key, rk);
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[160 + i]);
    for (int round = 9; round >= 0; round--) {
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[r + 4 * ((c + r) & 3)] = s[r + 4 * c];     // inverse shift rows
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(ISB[t[i]] ^ rk[16 * round + i]);
        if (round) {                                                                                         // inverse mix columns
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
                s[4 * c]     = (uint8_t)(gm(a0, 14) ^ gm(a1, 11) ^ gm(a2, 13) ^ gm(a3, 9));
                s[4 * c + 1] = (uint8_t)(gm(a0, 9) ^ gm(a1, 14) ^ gm(a2, 11) ^ gm(a3, 13));
                s[4 * c + 2] = (uint8_t)(gm(a0, 13) ^ gm(a1, 9) ^ gm(a2, 14) ^ gm(a3, 11));
                s[4 * c + 3] = (uint8_t)(gm(a0, 11) ^ gm(a1, 13) ^ gm(a2, 9) ^ gm(a3, 14));
            }
        }
    }
    memcpy(out, s, 16);
}

int wc_aes_unwrap(const uint8_t kek[16], const uint8_t* in, uint32_t n8, uint8_t* out) {
    if (n8 < 1 || n8 > 64) return -1;
    uint8_t a[8], r[64 * 8], b[16];
    memcpy(a, in, 8);
    memcpy(r, in + 8, n8 * 8);
    for (int j = 5; j >= 0; j--) {
        for (uint32_t i = n8; i >= 1; i--) {
            uint32_t t = n8 * (uint32_t)j + i;
            memcpy(b, a, 8);
            b[7] ^= (uint8_t)t; b[6] ^= (uint8_t)(t >> 8); b[5] ^= (uint8_t)(t >> 16); b[4] ^= (uint8_t)(t >> 24);
            memcpy(b + 8, r + (i - 1) * 8, 8);
            wc_aes128_decrypt_block(kek, b, b);
            memcpy(a, b, 8);
            memcpy(r + (i - 1) * 8, b + 8, 8);
        }
    }
    for (int i = 0; i < 8; i++) if (a[i] != 0xA6) return -1;
    memcpy(out, r, n8 * 8);
    return 0;
}

int wc_aes_wrap(const uint8_t kek[16], const uint8_t* in, uint32_t n8, uint8_t* out) {
    if (n8 < 2 || n8 > 64) return -1;
    aes128_ctx c;
    aes128_init(&c, kek);
    uint8_t a[8], b[16], r[64 * 8];
    memset(a, 0xA6, 8);
    memcpy(r, in, n8 * 8);
    for (uint32_t j = 0; j < 6; j++) {
        for (uint32_t i = 1; i <= n8; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, r + (i - 1) * 8, 8);
            aes128_encrypt_blocks(&c, b, b, 1);
            uint32_t t = n8 * j + i;
            memcpy(a, b, 8);
            a[7] ^= (uint8_t)t; a[6] ^= (uint8_t)(t >> 8); a[5] ^= (uint8_t)(t >> 16); a[4] ^= (uint8_t)(t >> 24);
            memcpy(r + (i - 1) * 8, b + 8, 8);
        }
    }
    memcpy(out, a, 8);
    memcpy(out + 8, r, n8 * 8);
    return 0;
}

// ---- AES-CCM (M = 8, L = 2) -------------------------------------------------------------------------------------------------------------
static void enc1(const aes128_ctx* c, const uint8_t in[16], uint8_t out[16]) { aes128_encrypt_blocks(c, in, out, 1); }

static void ccm_mac(const aes128_ctx* c, const uint8_t nonce[13], const uint8_t* aad, uint32_t aadlen, const uint8_t* msg, uint32_t len, uint8_t x[16]) {
    uint8_t b[16];
    b[0] = (uint8_t)((aadlen ? 0x40 : 0) | (((8 - 2) / 2) << 3) | 1);
    memcpy(b + 1, nonce, 13);
    b[14] = (uint8_t)(len >> 8); b[15] = (uint8_t)len;
    enc1(c, b, x);
    if (aadlen) {
        uint8_t blk[16];
        uint32_t pos = 0, first = 1;
        while (pos < aadlen || first) {
            memset(blk, 0, 16);
            uint32_t off = 0;
            if (first) { blk[0] = (uint8_t)(aadlen >> 8); blk[1] = (uint8_t)aadlen; off = 2; first = 0; }
            while (off < 16 && pos < aadlen) blk[off++] = aad[pos++];
            for (int i = 0; i < 16; i++) b[i] = (uint8_t)(x[i] ^ blk[i]);
            enc1(c, b, x);
        }
    }
    for (uint32_t pos = 0; pos < len; pos += 16) {
        uint32_t n = len - pos < 16 ? len - pos : 16;
        for (uint32_t i = 0; i < 16; i++) b[i] = (uint8_t)(x[i] ^ (i < n ? msg[pos + i] : 0));
        enc1(c, b, x);
    }
}

static void ccm_ctr(const aes128_ctx* c, const uint8_t nonce[13], uint32_t ctr, uint8_t s[16]) {
    uint8_t a[16];
    a[0] = 1;
    memcpy(a + 1, nonce, 13);
    a[14] = (uint8_t)(ctr >> 8); a[15] = (uint8_t)ctr;
    enc1(c, a, s);
}

void wc_ccm_encrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* aad, uint32_t aadlen, const uint8_t* in, uint32_t len, uint8_t* out, uint8_t mic[8]) {
    aes128_ctx c;
    aes128_init(&c, key);
    uint8_t x[16], s[16];
    ccm_mac(&c, nonce, aad, aadlen, in, len, x);
    ccm_ctr(&c, nonce, 0, s);
    for (int i = 0; i < 8; i++) mic[i] = (uint8_t)(x[i] ^ s[i]);
    for (uint32_t pos = 0, ctr = 1; pos < len; pos += 16, ctr++) {
        ccm_ctr(&c, nonce, ctr, s);
        uint32_t n = len - pos < 16 ? len - pos : 16;
        for (uint32_t i = 0; i < n; i++) out[pos + i] = (uint8_t)(in[pos + i] ^ s[i]);
    }
}

int wc_ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* aad, uint32_t aadlen, const uint8_t* in, uint32_t len, const uint8_t mic[8], uint8_t* out) {
    aes128_ctx c;
    aes128_init(&c, key);
    uint8_t s[16];
    for (uint32_t pos = 0, ctr = 1; pos < len; pos += 16, ctr++) {
        ccm_ctr(&c, nonce, ctr, s);
        uint32_t n = len - pos < 16 ? len - pos : 16;
        for (uint32_t i = 0; i < n; i++) out[pos + i] = (uint8_t)(in[pos + i] ^ s[i]);
    }
    uint8_t x[16];
    ccm_mac(&c, nonce, aad, aadlen, out, len, x);
    ccm_ctr(&c, nonce, 0, s);
    uint8_t diff = 0;
    for (int i = 0; i < 8; i++) diff |= (uint8_t)((x[i] ^ s[i]) ^ mic[i]);
    return diff ? -1 : 0;
}
