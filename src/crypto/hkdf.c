#include "hkdf.h"
#include "hmac.h"
#include <string.h>

// RFC 5869. PRK = HMAC-Hash(salt, IKM); T(i) = HMAC-Hash(PRK, T(i-1) || info || i)

void hkdf_extract(const uint8_t* salt, uint32_t salt_len,
                  const uint8_t* ikm, uint32_t ikm_len,
                  uint8_t prk[32]) {
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

int hkdf_expand(const uint8_t prk[32], const uint8_t* info, uint32_t info_len,
                uint8_t* okm, uint32_t okm_len) {
    // RFC 5869 §2.3: L <= 255*HashLen (counter is one octet). Refuse rather
    // than wrap the counter (a wrap would repeat keystream) or silently
    // clamp (review #40: a caller asking for 20KB must not get 8160 bytes
    // and a success code). In-tree uses are <= 64B — unreachable there.
    if (okm_len > 255 * 32) return -1;
    uint8_t t[32];
    uint32_t t_len = 0;
    uint32_t done = 0;
    uint8_t counter = 1;

    while (done < okm_len) {
        hmac_sha256_ctx ctx;
        hmac_sha256_init(&ctx, prk, 32);
        if (t_len) hmac_sha256_update(&ctx, t, t_len);
        if (info && info_len) hmac_sha256_update(&ctx, info, info_len);
        hmac_sha256_update(&ctx, &counter, 1);
        hmac_sha256_final(&ctx, t);

        uint32_t take = okm_len - done < 32 ? okm_len - done : 32;
        for (uint32_t i = 0; i < take; i++) okm[done + i] = t[i];
        done += take;
        t_len = 32;
        counter++;
    }
    return 0;
}

int hkdf(const uint8_t* salt, uint32_t salt_len,
         const uint8_t* ikm, uint32_t ikm_len,
         const uint8_t* info, uint32_t info_len,
         uint8_t* okm, uint32_t okm_len) {
    uint8_t prk[32];
    hkdf_extract(salt, salt_len, ikm, ikm_len, prk);
    int rc = hkdf_expand(prk, info, info_len, okm, okm_len);
    memset(prk, 0, sizeof(prk));
    return rc;
}
