// wjs_crypto.c - natives behind crypto.subtle (the SubtleCrypto class lives in the prelude): digests,
// HMAC, PBKDF2 and AES (CBC, CTR, GCM; 128/192/256-bit keys) over the kernel's own hash code. The AES here
// is the plain table version: it protects page data inside the page's own process, TLS keeps using the
// constant-time one in src/crypto/aes.c.

#include "wjs_int.h"
#include "../crypto/sha1.h"
#include "../crypto/sha256.h"
#include "../crypto/sha512.h"

#define JSX(ctx) ((struct wjs*)ojs_get_opaque(ctx))
#define NATIVE(name) static ojsv name(ojs* ctx, ojsv this_val, int argc, ojsv* argv)
#define UNUSED_THIS (void)this_val

#define MAX_DATA (64u << 20)

// ---- hashes: alg 0 = SHA-1, 1 = SHA-256, 2 = SHA-384, 3 = SHA-512 ----

static int hash_size(int alg) { return alg == 0 ? 20 : alg == 1 ? 32 : alg == 2 ? 48 : alg == 3 ? 64 : 0; }
static int hash_block(int alg) { return alg <= 1 ? 64 : 128; }

static void hash_once(int alg, const uint8_t* m, uint32_t n, uint8_t* out) {
    uint8_t t[64];
    switch (alg) {
    case 0: sha1(m, n, out); break;
    case 1: sha256(m, n, out); break;
    case 2: sha384(m, n, t); memcpy(out, t, 48); break;
    default: sha512(m, n, out); break;
    }
}

// streaming over two pieces (HMAC inner/outer pads + message)
struct hctx { int alg; sha1_ctx a; sha256_ctx b; sha512_ctx c; };
static void h_init(struct hctx* h, int alg) {
    h->alg = alg;
    if (alg == 0) sha1_init(&h->a);
    else if (alg == 1) sha256_init(&h->b);
    else if (alg == 2) sha384_init(&h->c);
    else sha512_init(&h->c);
}
static void h_update(struct hctx* h, const uint8_t* m, uint32_t n) {
    if (h->alg == 0) sha1_update(&h->a, m, n);
    else if (h->alg == 1) sha256_update(&h->b, m, n);
    else sha512_update(&h->c, m, n);
}
static void h_final(struct hctx* h, uint8_t* out) {
    uint8_t t[64];
    if (h->alg == 0) sha1_final(&h->a, out);
    else if (h->alg == 1) sha256_final(&h->b, out);
    else if (h->alg == 2) { sha512_final(&h->c, t); memcpy(out, t, 48); }
    else sha512_final(&h->c, out);
}

static void hmac(int alg, const uint8_t* key, uint32_t kl, const uint8_t* msg, uint32_t ml, uint8_t* out) {
    uint8_t k[128], pad[128];
    int bs = hash_block(alg), hs = hash_size(alg);
    memset(k, 0, sizeof k);
    if (kl > (uint32_t)bs) hash_once(alg, key, kl, k); else memcpy(k, key, kl);
    struct hctx h;
    uint8_t inner[64];
    for (int i = 0; i < bs; i++) pad[i] = k[i] ^ 0x36;
    h_init(&h, alg); h_update(&h, pad, (uint32_t)bs); h_update(&h, msg, ml); h_final(&h, inner);
    for (int i = 0; i < bs; i++) pad[i] = k[i] ^ 0x5c;
    h_init(&h, alg); h_update(&h, pad, (uint32_t)bs); h_update(&h, inner, (uint32_t)hs); h_final(&h, out);
}

static void pbkdf2(int alg, const uint8_t* pw, uint32_t pl, const uint8_t* salt, uint32_t sl, uint32_t iter, uint8_t* out, uint32_t outlen) {
    int hs = hash_size(alg);
    uint8_t* s = (uint8_t*)w_malloc(sl + 4);
    if (!s) return;
    memcpy(s, salt, sl);
    for (uint32_t blk = 1, done = 0; done < outlen; blk++, done += (uint32_t)hs) {
        uint8_t u[64], t[64];
        s[sl] = (uint8_t)(blk >> 24); s[sl + 1] = (uint8_t)(blk >> 16); s[sl + 2] = (uint8_t)(blk >> 8); s[sl + 3] = (uint8_t)blk;
        hmac(alg, pw, pl, s, sl + 4, u);
        memcpy(t, u, (size_t)hs);
        for (uint32_t i = 1; i < iter; i++) {
            hmac(alg, pw, pl, u, (uint32_t)hs, u);
            for (int k = 0; k < hs; k++) t[k] ^= u[k];
        }
        uint32_t n = outlen - done < (uint32_t)hs ? outlen - done : (uint32_t)hs;
        memcpy(out + done, t, n);
    }
    w_free(s);
}

// ---- AES ----

static uint8_t sbox[256], isbox[256];
static int aes_ready;

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static uint8_t gm(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
    return r;
}
static void aes_tables(void) {
    if (aes_ready) return;
    uint8_t p = 1, q = 1;
    do {                                                 // multiplicative inverse walk (generator 3)
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4));
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) isbox[sbox[i]] = (uint8_t)i;
    aes_ready = 1;
}

struct aes { uint8_t rk[240]; int nr; };

static int aes_key(struct aes* a, const uint8_t* key, int kl) {
    if (kl != 16 && kl != 24 && kl != 32) return -1;
    aes_tables();
    int nk = kl / 4;
    a->nr = nk + 6;
    memcpy(a->rk, key, (size_t)kl);
    uint8_t rc = 1;
    for (int i = nk; i < 4 * (a->nr + 1); i++) {
        uint8_t t[4];
        memcpy(t, a->rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            uint8_t x = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rc); t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[x];
            rc = xt(rc);
        } else if (nk > 6 && i % nk == 4) {
            for (int k = 0; k < 4; k++) t[k] = sbox[t[k]];
        }
        for (int k = 0; k < 4; k++) a->rk[4 * i + k] = a->rk[4 * (i - nk) + k] ^ t[k];
    }
    return 0;
}

static void add_rk(uint8_t* s, const uint8_t* rk) { for (int i = 0; i < 16; i++) s[i] ^= rk[i]; }

static void aes_enc(const struct aes* a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_rk(s, a->rk);
    for (int r = 1; r <= a->nr; r++) {
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
        for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) t[4 * c + rr] = s[4 * ((c + rr) & 3) + rr];   // ShiftRows
        if (r < a->nr) {
            for (int c = 0; c < 4; c++) {
                uint8_t* x = t + 4 * c;
                uint8_t a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
                s[4 * c] = (uint8_t)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
                s[4 * c + 1] = (uint8_t)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
                s[4 * c + 2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
                s[4 * c + 3] = (uint8_t)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
            }
        } else memcpy(s, t, 16);
        add_rk(s, a->rk + 16 * r);
    }
    memcpy(out, s, 16);
}

static void aes_dec(const struct aes* a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    memcpy(s, in, 16);
    add_rk(s, a->rk + 16 * a->nr);
    for (int r = a->nr - 1; r >= 0; r--) {
        for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) t[4 * ((c + rr) & 3) + rr] = s[4 * c + rr];   // InvShiftRows
        for (int i = 0; i < 16; i++) s[i] = isbox[t[i]];
        add_rk(s, a->rk + 16 * r);
        if (r > 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t* x = s + 4 * c;
                uint8_t a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
                x[0] = (uint8_t)(gm(a0, 14) ^ gm(a1, 11) ^ gm(a2, 13) ^ gm(a3, 9));
                x[1] = (uint8_t)(gm(a0, 9) ^ gm(a1, 14) ^ gm(a2, 11) ^ gm(a3, 13));
                x[2] = (uint8_t)(gm(a0, 13) ^ gm(a1, 9) ^ gm(a2, 14) ^ gm(a3, 11));
                x[3] = (uint8_t)(gm(a0, 11) ^ gm(a1, 13) ^ gm(a2, 9) ^ gm(a3, 14));
            }
        }
    }
    memcpy(out, s, 16);
}

// GHASH multiply: x = x * h in GF(2^128) (bit-by-bit, MSB first)
static void gmul(uint8_t x[16], const uint8_t h[16]) {
    uint8_t z[16] = {0}, v[16];
    memcpy(v, h, 16);
    for (int i = 0; i < 128; i++) {
        if ((x[i >> 3] >> (7 - (i & 7))) & 1) for (int k = 0; k < 16; k++) z[k] ^= v[k];
        int lsb = v[15] & 1;
        for (int k = 15; k > 0; k--) v[k] = (uint8_t)((v[k] >> 1) | (v[k - 1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1;
    }
    memcpy(x, z, 16);
}

static void ghash_upd(uint8_t y[16], const uint8_t h[16], const uint8_t* d, uint32_t n) {
    for (uint32_t i = 0; i < n; i += 16) {
        uint32_t m = n - i < 16 ? n - i : 16;
        for (uint32_t k = 0; k < m; k++) y[k] ^= d[i + k];
        gmul(y, h);
    }
}

static void inc32(uint8_t c[16]) { for (int i = 15; i >= 12; i--) if (++c[i]) break; }
// CTR counter increment over the low `bits` bits of the block
static void inc_ctr(uint8_t c[16], int bits) {
    for (int i = 15, left = bits; i >= 0 && left > 0; i--, left -= 8) {
        if (left >= 8) { if (++c[i]) return; }
        else { uint8_t mask = (uint8_t)((1u << left) - 1); uint8_t v = (uint8_t)((c[i] + 1) & mask); c[i] = (uint8_t)((c[i] & ~mask) | v); return; }
    }
}

static void ctr_crypt(const struct aes* a, uint8_t ctr[16], int bits, const uint8_t* in, uint8_t* out, uint32_t n) {
    uint8_t ks[16];
    for (uint32_t i = 0; i < n; i += 16) {
        aes_enc(a, ctr, ks);
        uint32_t m = n - i < 16 ? n - i : 16;
        for (uint32_t k = 0; k < m; k++) out[i + k] = in[i + k] ^ ks[k];
        if (bits == 32) inc32(ctr); else inc_ctr(ctr, bits);
    }
}

static void gcm_tag(const struct aes* a, const uint8_t h[16], const uint8_t j0[16], const uint8_t* aad, uint32_t al, const uint8_t* ct, uint32_t cl, uint8_t tag[16]) {
    uint8_t y[16] = {0}, lens[16], ek[16];
    ghash_upd(y, h, aad, al);
    ghash_upd(y, h, ct, cl);
    uint64_t ab = (uint64_t)al * 8, cb = (uint64_t)cl * 8;
    for (int i = 0; i < 8; i++) { lens[i] = (uint8_t)(ab >> (56 - 8 * i)); lens[8 + i] = (uint8_t)(cb >> (56 - 8 * i)); }
    ghash_upd(y, h, lens, 16);
    aes_enc(a, j0, ek);
    for (int i = 0; i < 16; i++) tag[i] = y[i] ^ ek[i];
}

// ---- natives ----

static uint8_t* bytes_arg(ojs* ctx, int argc, ojsv* argv, int i, size_t* len) {
    *len = 0;
    if (i >= argc || ojs_is_undefined(argv[i]) || ojs_is_null(argv[i])) return (uint8_t*)"";
    uint8_t* p = ojs_bytes(ctx, argv[i], len);
    return p ? p : 0;
}
static int int_arg(ojs* ctx, int argc, ojsv* argv, int i, int d) {
    int32_t v = d;
    if (i < argc && ojs_to_int32(ctx, &v, argv[i]) < 0) return d;
    return v;
}

// cryptoDigest(alg, data) -> ArrayBuffer | null
NATIVE(n_digest) {
    UNUSED_THIS;
    int alg = int_arg(ctx, argc, argv, 0, -1);
    size_t n;
    uint8_t* d = bytes_arg(ctx, argc, argv, 1, &n);
    if (!hash_size(alg) || !d || n > MAX_DATA) return OJS_NULL;
    uint8_t out[64];
    hash_once(alg, d, (uint32_t)n, out);
    return ojs_arraybuffer_copy(ctx, out, (size_t)hash_size(alg));
}

// cryptoHmac(alg, key, data) -> ArrayBuffer | null
NATIVE(n_hmac) {
    UNUSED_THIS;
    int alg = int_arg(ctx, argc, argv, 0, -1);
    size_t kl, n;
    uint8_t* k = bytes_arg(ctx, argc, argv, 1, &kl);
    uint8_t* d = bytes_arg(ctx, argc, argv, 2, &n);
    if (!hash_size(alg) || !k || !d || n > MAX_DATA) return OJS_NULL;
    uint8_t out[64];
    hmac(alg, k, (uint32_t)kl, d, (uint32_t)n, out);
    return ojs_arraybuffer_copy(ctx, out, (size_t)hash_size(alg));
}

// cryptoPbkdf2(alg, password, salt, iterations, bytes) -> ArrayBuffer | null
NATIVE(n_pbkdf2) {
    UNUSED_THIS;
    int alg = int_arg(ctx, argc, argv, 0, -1);
    size_t pl, sl;
    uint8_t* pw = bytes_arg(ctx, argc, argv, 1, &pl);
    uint8_t* salt = bytes_arg(ctx, argc, argv, 2, &sl);
    double it = 0, bl = 0;
    if (argc > 4) { ojs_to_number(ctx, &it, argv[3]); ojs_to_number(ctx, &bl, argv[4]); }
    if (!hash_size(alg) || !pw || !salt || it < 1 || it > 20000000 || bl < 0 || bl > 1 << 20 || sl > MAX_DATA) return OJS_NULL;
    uint8_t* out = (uint8_t*)w_malloc((size_t)bl + 64);
    if (!out) return OJS_NULL;
    pbkdf2(alg, pw, (uint32_t)pl, salt, (uint32_t)sl, (uint32_t)it, out, (uint32_t)bl);
    ojsv r = ojs_arraybuffer_copy(ctx, out, (size_t)bl);
    w_free(out);
    return r;
}

// cryptoAes(decrypt, mode, key, iv, data, aad, tagBytes, ctrBits) -> ArrayBuffer | null (bad key/padding/tag)
//   mode 0 = CBC (PKCS#7), 1 = CTR, 2 = GCM (the tag is appended to the ciphertext)
NATIVE(n_aes) {
    UNUSED_THIS;
    int dec = int_arg(ctx, argc, argv, 0, 0), mode = int_arg(ctx, argc, argv, 1, -1);
    size_t kl, il, n, al;
    uint8_t* key = bytes_arg(ctx, argc, argv, 2, &kl);
    uint8_t* iv = bytes_arg(ctx, argc, argv, 3, &il);
    uint8_t* d = bytes_arg(ctx, argc, argv, 4, &n);
    uint8_t* aad = bytes_arg(ctx, argc, argv, 5, &al);
    int tl = int_arg(ctx, argc, argv, 6, 16), cbits = int_arg(ctx, argc, argv, 7, 64);
    if (!key || !iv || !d || !aad || n > MAX_DATA) return OJS_NULL;
    struct aes a;
    if (aes_key(&a, key, (int)kl) < 0) return OJS_NULL;
    uint8_t* out = 0;
    size_t on = 0;
    if (mode == 0) {
        if (il != 16) return OJS_NULL;
        uint8_t prev[16];
        memcpy(prev, iv, 16);
        if (!dec) {
            on = (n / 16 + 1) * 16;
            out = (uint8_t*)w_malloc(on);
            if (!out) return OJS_NULL;
            uint8_t blk[16];
            for (size_t i = 0; i < on; i += 16) {
                size_t m = n > i ? (n - i < 16 ? n - i : 16) : 0;
                uint8_t padv = (uint8_t)(on - n);
                for (size_t k = 0; k < 16; k++) blk[k] = (uint8_t)((k < m ? d[i + k] : padv) ^ prev[k]);
                aes_enc(&a, blk, out + i);
                memcpy(prev, out + i, 16);
            }
        } else {
            if (n == 0 || n % 16) return OJS_NULL;
            out = (uint8_t*)w_malloc(n);
            if (!out) return OJS_NULL;
            for (size_t i = 0; i < n; i += 16) {
                uint8_t t[16];
                aes_dec(&a, d + i, t);
                for (int k = 0; k < 16; k++) out[i + (size_t)k] = (uint8_t)(t[k] ^ prev[k]);
                memcpy(prev, d + i, 16);
            }
            uint8_t pv = out[n - 1];
            int bad = pv == 0 || pv > 16 || pv > n;
            for (size_t k = 0; !bad && k < pv; k++) if (out[n - 1 - k] != pv) bad = 1;
            if (bad) { w_free(out); return OJS_NULL; }
            on = n - pv;
        }
    } else if (mode == 1) {
        if (il != 16 || cbits < 1 || cbits > 128) return OJS_NULL;
        uint8_t ctr[16];
        memcpy(ctr, iv, 16);
        on = n;
        out = (uint8_t*)w_malloc(on + 1);
        if (!out) return OJS_NULL;
        ctr_crypt(&a, ctr, cbits, d, out, (uint32_t)n);
    } else if (mode == 2) {
        if (il < 1 || il > 1024 || tl < 4 || tl > 16) return OJS_NULL;
        uint8_t h[16] = {0}, zero[16] = {0}, j0[16], tag[16];
        aes_enc(&a, zero, h);
        if (il == 12) { memcpy(j0, iv, 12); j0[12] = j0[13] = j0[14] = 0; j0[15] = 1; }
        else {
            uint8_t y[16] = {0}, lens[16] = {0};
            ghash_upd(y, h, iv, (uint32_t)il);
            uint64_t bits = (uint64_t)il * 8;
            for (int i = 0; i < 8; i++) lens[8 + i] = (uint8_t)(bits >> (56 - 8 * i));
            ghash_upd(y, h, lens, 16);
            memcpy(j0, y, 16);
        }
        uint8_t ctr[16];
        memcpy(ctr, j0, 16);
        inc32(ctr);
        if (!dec) {
            on = n + (size_t)tl;
            out = (uint8_t*)w_malloc(on + 1);
            if (!out) return OJS_NULL;
            ctr_crypt(&a, ctr, 32, d, out, (uint32_t)n);
            gcm_tag(&a, h, j0, aad, (uint32_t)al, out, (uint32_t)n, tag);
            memcpy(out + n, tag, (size_t)tl);
        } else {
            if (n < (size_t)tl) return OJS_NULL;
            size_t cl = n - (size_t)tl;
            gcm_tag(&a, h, j0, aad, (uint32_t)al, d, (uint32_t)cl, tag);
            uint8_t diff = 0;
            for (int i = 0; i < tl; i++) diff |= (uint8_t)(tag[i] ^ d[cl + (size_t)i]);
            if (diff) return OJS_NULL;
            on = cl;
            out = (uint8_t*)w_malloc(on + 1);
            if (!out) return OJS_NULL;
            ctr_crypt(&a, ctr, 32, d, out, (uint32_t)cl);
        }
    } else return OJS_NULL;
    ojsv r = ojs_arraybuffer_copy(ctx, out, on);
    w_free(out);
    return r;
}

static const struct ojs_func_entry crypto_funcs[] = {
    { "cryptoDigest", n_digest, 2 },
    { "cryptoHmac", n_hmac, 3 },
    { "cryptoPbkdf2", n_pbkdf2, 5 },
    { "cryptoAes", n_aes, 8 },
};

void wjs_crypto_install(struct wjs* js, ojsv natives) {
    ojs_set_functions(js->J, natives, crypto_funcs, (int)(sizeof crypto_funcs / sizeof crypto_funcs[0]));
}
