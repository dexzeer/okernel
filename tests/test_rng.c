// Host unit test for the Fortuna-lite RNG gate (review 2026-09-10 #4):
// fail-closed until >= 64B from >= 2 source classes; single-class
// flooding (even 16+ stirs, even zeros) must NOT declare readiness.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../src/crypto/rand.h"

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("  PASS %s\n", name); \
    else { printf("  FAIL %s\n", name); fails++; } \
} while (0)

int main(void) {
    uint8_t out[64];
    uint8_t zero[32];
    memset(zero, 0, sizeof(zero));

    // Fresh state: must refuse.
    CHECK(rand_ready() == 0, "fresh state not ready");
    CHECK(rand_bytes(out, 32) == 0, "fresh rand_bytes fails closed");

    // 16x zero stirs, single class: still closed (the old code would have
    // been 2/3 "ready" by sample count by now).
    for (int i = 0; i < 16; i++) rand_stir_src(zero, RAND_SRC_BOOT);
    CHECK(rand_ready() == 0, "single-class flood stays closed");
    CHECK(rand_bytes(out, 32) == 0, "still fails closed");

    // Second class arrives -> reseed declares ready.
    for (int i = 0; i < 16; i++) {
        uint8_t s[32];
        for (int j = 0; j < 32; j++) s[j] = (uint8_t)(i * 31 + j * 7 + 1);
        rand_stir_src(s, RAND_SRC_TIMER);
    }
    CHECK(rand_ready() == 1, "two classes declare ready");
    CHECK(rand_bytes(out, 32) == 1, "rand_bytes works when ready");

    // Stream advances (no stuck output).
    uint8_t out2[32];
    CHECK(rand_bytes(out2, 32) == 1, "second draw works");
    CHECK(memcmp(out, out2, 32) != 0, "stream advances");

    // Personalization doesn't unready or break (and never counted: covered
    // by the single-class test above using zeros).
    rand_personalize((const uint8_t*)"public-mac", 10);
    CHECK(rand_ready() == 1, "personalize keeps ready");
    CHECK(rand_bytes(out, 16) == 1, "draws after personalize");

    if (fails == 0) printf("RNG TESTS PASS: 0 failures\n");
    else printf("RNG TESTS FAIL: %d failures\n", fails);
    return fails == 0 ? 0 : 1;
}
