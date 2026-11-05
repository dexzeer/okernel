// TLS 1.2 helpers, see tls12.h.
#include "tls12.h"
#include "tls_handshake.h"
#include "hmac.h"
#include "aead.h"
#include "aes.h"
#include "ec.h"
#include "rsa.h"
#include "memwipe.h"
#include <string.h>

#define EXT_STATUS_REQUEST   5
#define EXT_ALPN             16
#define EXT_EMS              23
#define EXT_PRE_SHARED_KEY   41
#define EXT_SUPPORTED_VERS   43
#define EXT_KEY_SHARE        51
#define EXT_RENEG_INFO       0xFF01

static uint32_t u16be(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t u24be(const uint8_t* p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }

int tls12_suite_known(uint16_t s) {
    return s == TLS12_ECDHE_ECDSA_AES128_GCM || s == TLS12_ECDHE_RSA_AES128_GCM ||
           s == TLS12_ECDHE_ECDSA_AES256_GCM || s == TLS12_ECDHE_RSA_AES256_GCM ||
           s == TLS12_ECDHE_ECDSA_CHACHA20 || s == TLS12_ECDHE_RSA_CHACHA20;
}
int tls12_suite_is384(uint16_t s) { return s == TLS12_ECDHE_ECDSA_AES256_GCM || s == TLS12_ECDHE_RSA_AES256_GCM; }
int tls12_suite_is_ecdsa(uint16_t s) {
    return s == TLS12_ECDHE_ECDSA_AES128_GCM || s == TLS12_ECDHE_ECDSA_AES256_GCM || s == TLS12_ECDHE_ECDSA_CHACHA20;
}
static int suite_is_chacha(uint16_t s) { return s == TLS12_ECDHE_ECDSA_CHACHA20 || s == TLS12_ECDHE_RSA_CHACHA20; }
static int suite_is_aes256(uint16_t s) { return tls12_suite_is384(s); }
uint32_t tls12_suite_key_len(uint16_t s) { return (suite_is_chacha(s) || suite_is_aes256(s)) ? 32 : 16; }
uint32_t tls12_suite_iv_len(uint16_t s) { return suite_is_chacha(s) ? 12 : 4; }

// ---- transcript ----

void tls12_hash_init(tls12_hash* h, int is384) {
    h->is384 = is384;
    if (is384) sha384_init(&h->s384);
    else sha256_init(&h->s256);
}

void tls12_hash_update(tls12_hash* h, const uint8_t* d, uint32_t n) {
    if (h->is384) sha512_update(&h->s384, d, n);
    else sha256_update(&h->s256, d, n);
}

uint32_t tls12_hash_peek(const tls12_hash* h, uint8_t out[TLS12_MAX_HASH]) {
    if (h->is384) {
        sha512_ctx c = h->s384;
        uint8_t full[SHA512_HASH_SIZE];
        sha512_final(&c, full);
        memcpy(out, full, 48);
        return 48;
    }
    sha256_ctx c = h->s256;
    sha256_final(&c, out);
    return 32;
}

// ---- PRF ----

// HMAC over a || b with the suite's hash.
static void prf_hmac(int is384, const uint8_t* key, uint32_t key_len, const uint8_t* a, uint32_t a_len,
                     const uint8_t* b, uint32_t b_len, uint8_t* out) {
    uint8_t m[TLS12_MAX_HASH + 128];
    memcpy(m, a, a_len);
    if (b_len) memcpy(m + a_len, b, b_len);
    if (is384) hmac_sha384(key, key_len, m, a_len + b_len, out);
    else hmac_sha256(key, key_len, m, a_len + b_len, out);
    secure_zero(m, sizeof m);
}

int tls12_prf(int is384, const uint8_t* secret, uint32_t secret_len, const char* label,
              const uint8_t* seed, uint32_t seed_len, uint8_t* out, uint32_t out_len) {
    uint8_t ls[128], a[TLS12_MAX_HASH], block[TLS12_MAX_HASH];
    uint32_t hl = is384 ? 48 : 32, ll = (uint32_t)strlen(label);
    if (ll > sizeof ls || seed_len > sizeof ls - ll) return -1;
    memcpy(ls, label, ll);
    memcpy(ls + ll, seed, seed_len);
    uint32_t lsl = ll + seed_len;
    prf_hmac(is384, secret, secret_len, ls, lsl, 0, 0, a);        // A(1)
    for (uint32_t off = 0; off < out_len; off += hl) {
        prf_hmac(is384, secret, secret_len, a, hl, ls, lsl, block);
        uint32_t k = out_len - off < hl ? out_len - off : hl;
        memcpy(out + off, block, k);
        prf_hmac(is384, secret, secret_len, a, hl, 0, 0, a);      // A(i+1)
    }
    secure_zero(ls, sizeof ls);
    secure_zero(a, sizeof a);
    secure_zero(block, sizeof block);
    return 0;
}

int tls12_master(int is384, int ems, const uint8_t* pms, uint32_t pms_len,
                 const uint8_t* session_hash, uint32_t hash_len,
                 const uint8_t client_random[32], const uint8_t server_random[32],
                 uint8_t out[TLS12_MASTER_LEN]) {
    if (ems) return tls12_prf(is384, pms, pms_len, "extended master secret", session_hash, hash_len, out, TLS12_MASTER_LEN);
    uint8_t seed[64];
    memcpy(seed, client_random, 32);
    memcpy(seed + 32, server_random, 32);
    return tls12_prf(is384, pms, pms_len, "master secret", seed, 64, out, TLS12_MASTER_LEN);
}

// ---- ServerHello ----

// Finds the extension block. 0 with *p at its first extension and *end past
// the last one (*has_block 0 when the message simply ends after compression).
static int sh_ext_block(const uint8_t* sh, uint32_t len, uint32_t* p, uint32_t* end, int* has_block) {
    if (len < 2 + 32 + 1) return -1;
    uint32_t q = 2 + 32;
    uint32_t sid = sh[q++];
    if (sid > 32 || len < q + sid + 2 + 1) return -1;
    q += sid + 2 + 1;
    *has_block = 0;
    if (q == len) { *p = *end = q; return 0; }
    if (len < q + 2) return -1;
    uint32_t el = u16be(sh + q);
    q += 2;
    if (q + el != len) return -1;
    *p = q;
    *end = q + el;
    *has_block = 1;
    return 0;
}

int tls12_sh_version(const uint8_t* sh, uint32_t len) {
    uint32_t p, end;
    int has;
    if (sh_ext_block(sh, len, &p, &end, &has) != 0) return -1;
    while (p + 4 <= end) {
        uint32_t t = u16be(sh + p), l = u16be(sh + p + 2);
        p += 4;
        if (l > end - p) return -1;
        if (t == EXT_SUPPORTED_VERS) return 13;
        p += l;
    }
    return p == end ? 12 : -1;
}

int tls12_parse_server_hello(const uint8_t* sh, uint32_t len, const uint8_t our_sid[32], int allow_h2,
                             struct tls12_sh* out) {
    static const uint8_t downgrade[7] = { 'D', 'O', 'W', 'N', 'G', 'R', 'D' };
    uint32_t p, end;
    int has;
    memset(out, 0, sizeof *out);
    if (sh_ext_block(sh, len, &p, &end, &has) != 0 || !has) return -1;   // no extensions = no renegotiation_info
    if (u16be(sh) != 0x0303) return -1;
    out->random = sh + 2;
    // A real 1.3 server talking to a client that offers 1.3 never negotiates
    // 1.2, so this mark means somebody removed 1.3 from our hello.
    if (memcmp(sh + 2 + 24, downgrade, 7) == 0 && (sh[2 + 31] == 0 || sh[2 + 31] == 1)) return -1;
    uint32_t q = 2 + 32;
    uint32_t sid = sh[q++];
    if (sid == 32 && memcmp(sh + q, our_sid, 32) == 0) return -1;   // "resuming" a session we never had
    q += sid;
    out->suite = (uint16_t)u16be(sh + q);
    q += 2;
    if (!tls12_suite_known(out->suite)) return -1;
    if (sh[q] != 0) return -1;                                      // compression
    q++;
    // Framing, duplicates and the ALPN answer: same rules as EncryptedExtensions.
    if (tls_parse_ee_validate_alpn(sh + q, len - q, allow_h2, &out->alpn_h2) != 0) return -1;
    int ri = 0;
    while (p + 4 <= end) {
        uint32_t t = u16be(sh + p), l = u16be(sh + p + 2);
        p += 4;
        if (t == EXT_RENEG_INFO) {
            if (l != 1 || sh[p] != 0) return -1;
            ri = 1;
        } else if (t == EXT_EMS) {
            if (l != 0) return -1;
            out->ems = 1;
        } else if (t == EXT_STATUS_REQUEST) {
            if (l != 0) return -1;
            out->status = 1;
        } else if (t == EXT_KEY_SHARE || t == EXT_PRE_SHARED_KEY) {
            return -1;
        }
        p += l;
    }
    return ri ? 0 : -1;
}

// ---- server flight ----

int tls12_cert_to_13(const uint8_t* m, uint32_t len, uint8_t* out, uint32_t cap, uint32_t* out_len) {
    if (len < 3 || u24be(m) != len - 3 || len == 3) return -1;      // an empty chain authenticates nobody
    if (cap < 4) return -1;
    uint32_t p = 3, o = 4;
    while (p < len) {
        if (len - p < 3) return -1;
        uint32_t cl = u24be(m + p);
        if (cl == 0 || cl > len - p - 3) return -1;
        if (cap - o < 3 + cl + 2) return -1;
        memcpy(out + o, m + p, 3 + cl);
        o += 3 + cl;
        out[o++] = 0;
        out[o++] = 0;
        p += 3 + cl;
    }
    uint32_t list = o - 4;
    out[0] = 0;
    out[1] = (uint8_t)(list >> 16);
    out[2] = (uint8_t)(list >> 8);
    out[3] = (uint8_t)list;
    *out_len = o;
    return 0;
}

int tls12_parse_cert_status(const uint8_t* m, uint32_t len, const uint8_t** resp, uint32_t* resp_len) {
    if (len < 4 || m[0] != 1) return -1;                            // 1 = ocsp
    uint32_t rl = u24be(m + 1);
    if (rl == 0 || rl != len - 4) return -1;
    *resp = m + 4;
    *resp_len = rl;
    return 0;
}

int tls12_parse_ske(const uint8_t* m, uint32_t len, uint16_t* group, uint32_t* params_len, uint16_t* alg,
                    const uint8_t** sig, uint32_t* sig_len) {
    if (len < 4 || m[0] != 3) return -1;                            // named_curve
    uint16_t g = (uint16_t)u16be(m + 1);
    uint32_t kl = g == TLS_GROUP_X25519 ? 32 : ec_ecdh_point_len(g);   // the groups we offer
    if (kl == 0 || m[3] != kl) return -1;
    uint32_t pl = 4 + kl;
    if (len < pl + 4) return -1;
    *group = g;
    *params_len = pl;
    *alg = (uint16_t)u16be(m + pl);
    uint32_t sl = u16be(m + pl + 2);
    if (sl == 0 || sl != len - pl - 4) return -1;
    *sig = m + pl + 4;
    *sig_len = sl;
    return 0;
}

int tls12_verify_ske(uint16_t suite, const x509_cert* leaf, uint16_t alg,
                     const uint8_t client_random[32], const uint8_t server_random[32],
                     const uint8_t* params, uint32_t params_len,
                     const uint8_t* sig, uint32_t sig_len) {
    uint8_t d[64 + TLS12_SKE_PARAMS_MAX];
    if (params_len > TLS12_SKE_PARAMS_MAX) return -1;
    uint32_t dl = 64 + params_len;
    memcpy(d, client_random, 32);
    memcpy(d + 32, server_random, 32);
    memcpy(d + 64, params, params_len);
    int is_ec = leaf->key_type == X509_KEY_EC_P256 || leaf->key_type == X509_KEY_EC_P384;
    if (tls12_suite_is_ecdsa(suite)) {
        if (!is_ec) return -1;
        // In 1.2 these codes name a hash, not a curve: a P-384 key may use SHA-256.
        if (alg == 0x0403) return ec_verify(X509_SIG_ECDSA_SHA256, leaf->ec_point, leaf->ec_point_len, d, dl, sig, sig_len);
        if (alg == 0x0503) return ec_verify(X509_SIG_ECDSA_SHA384, leaf->ec_point, leaf->ec_point_len, d, dl, sig, sig_len);
        return -1;
    }
    if (leaf->key_type != X509_KEY_RSA) return -1;
    rsa_pub k;
    if (rsa_pub_from_x509(leaf, &k) != 0) return -1;
    switch (alg) {
    case 0x0401: return rsa_verify_pkcs1(&k, X509_SIG_RSA_SHA256, d, dl, sig, sig_len);
    case 0x0501: return rsa_verify_pkcs1(&k, X509_SIG_RSA_SHA384, d, dl, sig, sig_len);
    case 0x0601: return rsa_verify_pkcs1(&k, X509_SIG_RSA_SHA512, d, dl, sig, sig_len);
    case 0x0804: return rsa_verify_pss(&k, X509_SIG_RSA_SHA256, d, dl, sig, sig_len);
    case 0x0805: return rsa_verify_pss(&k, X509_SIG_RSA_SHA384, d, dl, sig, sig_len);
    }
    return -1;
}

// ---- records ----

static void rec_nonce(uint16_t suite, const uint8_t* iv, uint64_t seq, uint8_t nonce[12]) {
    if (suite_is_chacha(suite)) {
        memcpy(nonce, iv, 12);
        for (int i = 0; i < 8; i++) nonce[4 + i] ^= (uint8_t)(seq >> (56 - 8 * i));
    } else {
        memcpy(nonce, iv, 4);
        for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(seq >> (56 - 8 * i));
    }
}

static void rec_aad(uint8_t aad[13], uint64_t seq, uint8_t type, uint32_t pt_len) {
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)(seq >> (56 - 8 * i));
    aad[8] = type;
    aad[9] = 3;
    aad[10] = 3;
    aad[11] = (uint8_t)(pt_len >> 8);
    aad[12] = (uint8_t)pt_len;
}

int tls12_seal(uint16_t suite, const uint8_t* key, const uint8_t* iv, uint64_t seq, uint8_t type,
               const uint8_t* pt, uint32_t pt_len, uint8_t* out, uint32_t* out_len) {
    if (pt_len > TLS12_MAX_PLAIN || seq == 0xFFFFFFFFFFFFFFFFull) return -1;
    uint8_t nonce[12], aad[13];
    rec_nonce(suite, iv, seq, nonce);
    rec_aad(aad, seq, type, pt_len);
    uint32_t ex = 0;
    if (!suite_is_chacha(suite)) {       // the explicit part of the GCM nonce goes on the wire
        memcpy(out, nonce + 4, 8);
        ex = 8;
    }
    int rc;
    if (suite_is_chacha(suite)) rc = aead_chacha20_poly1305_encrypt(key, nonce, aad, 13, pt, pt_len, out, out + pt_len);
    else if (suite_is_aes256(suite)) rc = aead_aes256gcm_encrypt(key, nonce, aad, 13, pt, pt_len, out + ex, out + ex + pt_len);
    else rc = aead_aes128gcm_encrypt(key, nonce, aad, 13, pt, pt_len, out + ex, out + ex + pt_len);
    secure_zero(nonce, sizeof nonce);
    if (rc != 0) return -1;
    *out_len = ex + pt_len + 16;
    return 0;
}

int tls12_open(uint16_t suite, const uint8_t* key, const uint8_t* iv, uint64_t seq, uint8_t type,
               const uint8_t* rec, uint32_t rec_len, uint8_t* out, uint32_t* out_len) {
    uint32_t ex = suite_is_chacha(suite) ? 0 : 8;
    if (rec_len < ex + 16 || seq == 0xFFFFFFFFFFFFFFFFull) return -1;
    uint32_t pt_len = rec_len - ex - 16;
    if (pt_len > TLS12_MAX_PLAIN) return -1;
    uint8_t nonce[12], aad[13];
    rec_nonce(suite, iv, seq, nonce);
    if (ex) memcpy(nonce + 4, rec, 8);   // the sender picks it; the tag covers the choice
    rec_aad(aad, seq, type, pt_len);
    int rc;
    if (suite_is_chacha(suite)) rc = aead_chacha20_poly1305_decrypt(key, nonce, aad, 13, rec, pt_len, rec + pt_len, out);
    else if (suite_is_aes256(suite)) rc = aead_aes256gcm_decrypt(key, nonce, aad, 13, rec + ex, pt_len, rec + ex + pt_len, out);
    else rc = aead_aes128gcm_decrypt(key, nonce, aad, 13, rec + ex, pt_len, rec + ex + pt_len, out);
    secure_zero(nonce, sizeof nonce);
    if (rc != 0) return -1;
    *out_len = pt_len;
    return 0;
}
