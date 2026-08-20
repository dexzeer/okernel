#include <stdio.h>
#include <string.h>
#include "sha256.h"

static void hexprint(const char* label, const uint8_t* h, int len) {
    printf("%-40s", label);
    for (int i = 0; i < len; i++) printf("%02x", h[i]);
    printf("\n");
}

static int mem_eq(const uint8_t* a, const uint8_t* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int main(void) {
    uint8_t hash[32];
    int ok = 1;

    // Empty string
    sha256((const uint8_t*)"", 0, hash);
    uint8_t empty[] = { 0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
                        0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
                        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,
                        0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55 };
    hexprint("empty:", hash, 32);
    ok &= mem_eq(hash, empty, 32);

    // "abc"
    sha256((const uint8_t*)"abc", 3, hash);
    uint8_t abc[] = { 0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
                      0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
                      0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
                      0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
    hexprint("abc:", hash, 32);
    ok &= mem_eq(hash, abc, 32);

    // 448-bit message (one-block boundary test)
    const char* msg_56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256((const uint8_t*)msg_56, strlen(msg_56), hash);
    uint8_t v56[] = { 0x24,0x8d,0x6a,0x61,0xd2,0x06,0x38,0xb8,
                      0xe5,0xc0,0x26,0x93,0x0c,0x3e,0x60,0x39,
                      0xa3,0x3c,0xe4,0x59,0x64,0xff,0x21,0x67,
                      0xf6,0xec,0xed,0xd4,0x19,0xdb,0x06,0xc1 };
    hexprint("56chars:", hash, 32);
    ok &= mem_eq(hash, v56, 32);

    // Streaming test (same as abc but via update calls)
    {
        sha256_ctx ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, (const uint8_t*)"a", 1);
        sha256_update(&ctx, (const uint8_t*)"bc", 2);
        sha256_final(&ctx, hash);
        printf("abc(stream): %s\n", mem_eq(hash, abc, 32) ? "PASS" : "FAIL");
        ok &= mem_eq(hash, abc, 32);
    }

    printf("\n%s\n", ok ? "ALL SHA-256 TESTS PASSED" : "FAILURE");
    return !ok;
}
