// Host unit test for the Fortuna-lite RNG gate (review 2026-09-10 #4,
// tiers cryptoholes #1): fail-closed until a declaring reseed compresses
// enough window bytes from enough classes. Host tests never call
// rand_hw_init, so hw_present stays 0 → the no-hardware tier applies:
// >= 128B window, >= 2 window classes at >= 32B each, >= 3 lifetime
// classes (BOOT+TIMER+INPUT). Single-class flooding (even 16+ stirs,
// even zeros) must NOT declare readiness; neither must two weak classes
// without the third, nor a lone weak sample piggybacking on bulk.
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

    // Second class arrives, but the no-hardware tier needs THREE lifetime
    // classes — still closed (this is the cryptoholes #1 tightening: two
    // weak label-only classes no longer suffice).
    for (int i = 0; i < 16; i++) {
        uint8_t s[32];
        for (int j = 0; j < 32; j++) s[j] = (uint8_t)(i * 31 + j * 7 + 1);
        rand_stir_src(s, RAND_SRC_TIMER);
    }
    CHECK(rand_ready() == 0, "two classes still closed (need three, no hw)");
    CHECK(rand_bytes(out, 32) == 0, "still fails closed");

    // Third class arrives interleaved (production reality: TIMER ticks
    // constantly, so any INPUT lands in a mixed window — segregated
    // 16-batches would reseed single-class windows and legitimately wait).
    for (int i = 0; i < 8; i++) {
        uint8_t s[32], t[32];
        for (int j = 0; j < 32; j++) {
            s[j] = (uint8_t)(i * 13 + j * 3 + 2);
            t[j] = (uint8_t)(i * 17 + j * 5 + 3);
        }
        rand_stir_src(s, RAND_SRC_INPUT);
        rand_stir_src(t, RAND_SRC_TIMER);
    }
    CHECK(rand_ready() == 1, "three classes declare ready");
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

    // State transitions (cryptoholes #9): soak across many reseeds —
    // readiness latches, the stream keeps advancing (no stuck state, no
    // reseed-window deadlock), draws stay usable.
    {
        uint8_t prev[32], cur[32];
        CHECK(rand_bytes(prev, 32) == 1, "soak baseline draw");
        int stuck = 0;
        for (int i = 0; i < 200; i++) {
            uint8_t s[32];
            for (int j = 0; j < 32; j++) s[j] = (uint8_t)(i * 29 + j * 11 + 5);
            // cycle classes so every reseed window stays mixed
            rand_stir_src(s, (i & 1) ? RAND_SRC_TIMER : RAND_SRC_INPUT);
            if (rand_bytes(cur, 32) != 1) { stuck = -1; break; }
            if (memcmp(prev, cur, 32) == 0) { stuck = 1; break; }
            memcpy(prev, cur, 32);
        }
        CHECK(stuck == 0, "200 reseed-crossing draws all advance");
        CHECK(rand_ready() == 1, "ready latches across reseeds");
    }

    // Hardware path smoke: rand_hw_init must never crash and must leave
    // the state coherent (ready stays, draws advance) whether the host
    // has RDRAND/RDSEED or not. (True failure injection — CPUID without
    // RDRAND, partial pulls — is not controllable from userspace; the
    // no-hw tier above is the enforced fallback and is fully tested.)
    {
        uint8_t before[32], after[32];
        CHECK(rand_bytes(before, 32) == 1, "pre-hw draw");
        int hw = rand_hw_init();
        CHECK(hw == 0 || hw == 1, "hw_init returns sane value");
        CHECK(rand_ready() == 1, "ready survives hw_init");
        CHECK(rand_bytes(after, 32) == 1, "post-hw draw");
        CHECK(memcmp(before, after, 32) != 0, "hw stir advances stream");
    }

    if (fails == 0) printf("RNG TESTS PASS: 0 failures\n");
    else printf("RNG TESTS FAIL: %d failures\n", fails);
    return fails == 0 ? 0 : 1;
}
