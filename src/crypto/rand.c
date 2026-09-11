// Fortuna-lite ChaCha20 CPRNG (see rand.h for the entropy model).
#include "rand.h"
#include "chacha20.h"
#include "sha256.h"
#include <string.h>

#ifdef KERNEL
#define RNG_CLI() __asm__ volatile("cli" ::: "memory")
#define RNG_STI() __asm__ volatile("sti" ::: "memory")
#else
#define RNG_CLI() do {} while (0)
#define RNG_STI() do {} while (0)
#endif

static uint8_t  rng_key[32];
static uint8_t  rng_nonce[12];
static uint32_t rng_counter;
static int      rng_ready_flag;
static int      hw_present; // a hardware-RNG class was actually stirred

// Pools: 16 samples x 32B each (512B per pool — BSS-cheap). pool_src tracks
// which classes contributed since the last reseed (diversity gate).
#define RNG_POOL_SLOTS 16
static uint8_t  pool0[RNG_POOL_SLOTS][32];
static uint8_t  pool1[RNG_POOL_SLOTS][32];
static uint32_t pool0_n, pool1_n, pool1_tick;
static int      pool_src_mask;
// Freshness gate (cryptoholes #1): bytes per class in the CURRENT pool0
// window. The declaring reseed needs >= 2 classes with >= 32B each IN
// THAT WINDOW — lifetime presence alone is not enough (a lone weak
// sample must not piggyback on ancient bulk).
static uint32_t pool_cls_win[RAND_NCLASS];

// 64 bytes of keystream used as scratch to extract new key material.
static void rng_rekey(void) {
    uint8_t block[64];
    chacha20_encrypt(rng_key, rng_nonce, rng_counter, block, block, 64);
    rng_counter++;

    // Mix: new key = old key XOR first 32 bytes of block.
    for (int i = 0; i < 32; i++) rng_key[i] ^= block[i];

    // Overwrite the block; we only needed the first 32 bytes.
    memset(block, 0, sizeof(block));
}

// Compress `n` pool slots through SHA-256 into out32. The hash is the
// conditioner: arbitrary attacker-influenced samples collapse to 256
// unpredictable bits before touching the key.
static void pool_compress(uint8_t pool[RNG_POOL_SLOTS][32], uint32_t n,
                          uint8_t out32[32]) {
    sha256_ctx ctx;
    sha256_init(&ctx);
    for (uint32_t i = 0; i < n && i < RNG_POOL_SLOTS; i++)
        sha256_update(&ctx, pool[i], 32);
    sha256_final(&ctx, out32);
}

static void pools_wipe(void) {
    // Contents wipe every reseed; the SOURCE MASK persists for the machine's
    // lifetime (diversity is a lifetime property for the first honest seed:
    // wiping it per-window deadlocks readiness whenever classes arrive in
    // segregated batches — e.g. 16 BOOT stirs, reseed, then TIMER only).
    // The per-class WINDOW bytes reset (freshness is per-window by design).
    memset(pool0, 0, sizeof(pool0));
    memset(pool1, 0, sizeof(pool1));
    pool0_n = pool1_n = 0;
    memset(pool_cls_win, 0, sizeof(pool_cls_win));
}

// Fold pools into the key. Every 4th reseed also folds the slow pool.
// Readiness (first honest seed, cryptoholes #1 tiers): the declaring
// reseed's pool0 window must hold >= 64B (>= 128B with no hardware RNG),
// with >= 2 classes at >= 32B each IN THAT WINDOW, and lifetime classes
// >= 2 including hardware (or >= 3 = BOOT+TIMER+INPUT without hardware).
static void rng_reseed(void) {
    static uint32_t reseed_gen = 0;
    uint8_t digest[32];
    pool_compress(pool0, pool0_n, digest);
    for (int i = 0; i < 32; i++) rng_key[i] ^= digest[i];
    reseed_gen++;
    if ((reseed_gen & 3) == 0 && pool1_n > 0) {
        pool_compress(pool1, pool1_n, digest);
        for (int i = 0; i < 32; i++) rng_key[i] ^= digest[i];
    }
    memset(digest, 0, sizeof(digest));
    rng_rekey();
    rng_counter++; // domain-separate post-reseed stream
    if (!rng_ready_flag) {
        uint32_t need_bytes = hw_present ? 64 : 128;
        if (pool0_n * 32 >= need_bytes) {
            int life = 0, win = 0;
            for (int b = 0; b < RAND_NCLASS; b++) {
                if (pool_src_mask & (1 << b)) life++;
                if (pool_cls_win[b] >= 32) win++;
            }
            int need_life = hw_present ? 2 : 3;
            int hw_ok = !hw_present ||
                (pool_src_mask & (RAND_SRC_RDRAND | RAND_SRC_RDSEED));
            if (life >= need_life && hw_ok && win >= 2) rng_ready_flag = 1;
        }
    }
    pools_wipe();
}

void rand_stir_src(const uint8_t entropy[32], int src) {
    RNG_CLI();
    src &= (RAND_SRC_RDSEED | RAND_SRC_RDRAND | RAND_SRC_INPUT |
            RAND_SRC_TIMER | RAND_SRC_BOOT); // 0x1F: no invented classes
    if (pool0_n < RNG_POOL_SLOTS) {
        memcpy(pool0[pool0_n], entropy, 32);
        pool0_n++;
        for (int b = 0; b < RAND_NCLASS; b++)
            if (src & (1 << b)) pool_cls_win[b] += 32;
    }
    pool1_tick++;
    if ((pool1_tick & 15) == 0 && pool1_n < RNG_POOL_SLOTS) {
        memcpy(pool1[pool1_n], entropy, 32);
        pool1_n++;
    }
    pool_src_mask |= src;
    // Reseed when the fast pool fills; the pools then start over.
    if (pool0_n >= RNG_POOL_SLOTS) rng_reseed();
    RNG_STI();
}

void rand_stir(const uint8_t entropy[32]) {
    rand_stir_src(entropy, RAND_SRC_TIMER);
}

void rand_seed(const uint8_t entropy[32]) {
    RNG_CLI();
    // A seed is one BOOT-class sample plus an immediate reseed attempt
    // (which will not declare readiness — single class by construction).
    if (pool0_n < RNG_POOL_SLOTS) {
        memcpy(pool0[pool0_n], entropy, 32);
        pool0_n++;
        pool_cls_win[0] += 32; // RAND_SRC_BOOT is bit 0
    }
    pool_src_mask |= RAND_SRC_BOOT;
    rng_reseed();
    RNG_STI();
}

void rand_personalize(const uint8_t* data, uint32_t len) {
    // Domain separation only: folded straight into the key, never counted
    // toward readiness, never mistaken for entropy (the MAC lesson).
    RNG_CLI();
    for (uint32_t i = 0; i < len; i++) rng_key[i % 32] ^= data[i];
    rng_rekey();
    RNG_STI();
}

// ---- x86 hardware RNG (opportunistic) ----

static void cpuid32(uint32_t leaf, uint32_t* a, uint32_t* b,
                    uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

// RDRAND EAX (32-bit): CF=1 on success. Bounded retries per word.
static int rdrand32(uint32_t* out) {
    uint8_t ok = 0;
    uint32_t v = 0;
    for (int i = 0; i < 10; i++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

// RDSEED EAX (32-bit): CF=1 on success. Unlike RDRAND it can legitimately
// fail when the conditioner needs reseeding — bounded retries with PAUSE
// backoff per Intel guidance (still boot-time-cheap).
static int rdseed32(uint32_t* out) {
    uint8_t ok = 0;
    uint32_t v = 0;
    for (int i = 0; i < 64; i++) {
        __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return 1; }
        __asm__ volatile("pause");
    }
    return 0;
}

static int hw_bytes(int cls, int (*pull)(uint32_t*)) {
    uint8_t hw[32];
    for (int i = 0; i < 8; i++) {
        uint32_t w = 0;
        if (!pull(&w)) return 0;
        hw[4*i] = (uint8_t)(w & 0xff); hw[4*i+1] = (uint8_t)((w >> 8) & 0xff);
        hw[4*i+2] = (uint8_t)((w >> 16) & 0xff); hw[4*i+3] = (uint8_t)(w >> 24);
    }
    rand_stir_src(hw, cls);
    memset(hw, 0, sizeof(hw));
    hw_present = 1; // hardware bytes actually mixed (not just CPUID claims)
    return 1;
}

int rand_hw_init(void) {
    uint32_t a, b, c, d;
    int got = 0;
    // RDSEED first (conditioned output, stronger contract), then RDRAND.
    // Either (or both) establishes the hardware tier. CPUID leaf 7 needs
    // max-leaf guard (pre-2013 CPUs return garbage, not faults — check).
    cpuid32(0, &a, &b, &c, &d);
    if (a >= 7) {
        cpuid32(7, &a, &b, &c, &d);
        if (b & (1u << 18))
            got |= hw_bytes(RAND_SRC_RDSEED, rdseed32);
    }
    cpuid32(1, &a, &b, &c, &d);
    if (c & (1u << 30))
        got |= hw_bytes(RAND_SRC_RDRAND, rdrand32);
    return got; // 0 = no hardware RNG: pools-only tiers (logged by caller)
}

int rand_bytes(uint8_t* out, uint32_t len) {
    int rc = 0;
    RNG_CLI();
    if (!rng_ready_flag) goto out;
    // Snapshot divergence: fresh RDTSC folded into the nonce per call, so
    // two boots/snapshots with identical pools still diverge immediately.
    {
        uint64_t t = 0;
        __asm__ volatile("rdtsc" : "=A"(t));
        for (int i = 0; i < 8; i++)
            rng_nonce[i] ^= (uint8_t)((t >> (8 * i)) & 0xFF);
    }
    while (len > 0) {
        // Generate 64 bytes of keystream, use them, then rekey.
        uint8_t block[64];
        chacha20_encrypt(rng_key, rng_nonce, rng_counter, block, block, 64);
        rng_counter++;

        uint32_t take = len < 64 ? len : 64;
        memcpy(out, block, take);
        out += take;
        len -= take;

        // Forward secrecy: mix used keystream into key.
        for (uint32_t i = 0; i < 32; i++) rng_key[i] ^= block[i];

        memset(block, 0, sizeof(block));
        rng_rekey();
    }
    rc = 1;
out:
    RNG_STI();
    return rc;
}

int rand_ready(void) {
    return rng_ready_flag;
}
