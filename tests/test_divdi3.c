// test_divdi3.c — src/divdi3.c (our 64-bit division helpers) against the host
// compiler's own division on edge cases and random operands of every magnitude.
//   gcc -m32 -O2 -o build-host/t_divdi3 tests/test_divdi3.c && ./build-host/t_divdi3 [n]
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define __udivmoddi4 my_udivmoddi4
#define __udivdi3 my_udivdi3
#define __umoddi3 my_umoddi3
#define __divmoddi4 my_divmoddi4
#define __divdi3 my_divdi3
#define __moddi3 my_moddi3
#include "../src/divdi3.c"

static uint64_t s = 0x2545F4914F6CDD1Dull;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static uint64_t shaped(void) {   // random value with a random bit length
    uint64_t v = rnd();
    int bits = (int)(rnd() % 65);
    return bits == 64 ? v : v & ((1ull << bits) - 1);
}

int main(int argc, char** argv) {
    long n = argc > 1 ? atol(argv[1]) : 20000000, bad = 0;
    static const uint64_t edge[] = { 0, 1, 2, 3, 7, 0xFFFFFFFFull, 0x100000000ull, 0x100000001ull, 0x7FFFFFFFFFFFFFFFull,
                                     0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFF00000000ull, 1000000007ull, 10, 0x80000000ull };
    int ne = (int)(sizeof edge / sizeof edge[0]);
    for (long i = 0; i < n + ne * ne; i++) {
        uint64_t a, b;
        if (i < ne * ne) { a = edge[i / ne]; b = edge[i % ne]; }
        else { a = shaped(); b = shaped(); }
        if (!b) continue;
        if (my_udivdi3(a, b) != a / b || my_umoddi3(a, b) != a % b) {
            if (bad++ < 10) printf("u %llx / %llx\n", (unsigned long long)a, (unsigned long long)b);
        }
        int64_t sa = (int64_t)a, sb = (int64_t)b;
        if (sb == -1 && sa == INT64_MIN) continue;   // overflows in C too
        if (my_divdi3(sa, sb) != sa / sb || my_moddi3(sa, sb) != sa % sb) {
            if (bad++ < 10) printf("s %lld / %lld\n", (long long)sa, (long long)sb);
        }
    }
    printf("divdi3: %ld cases, %ld mismatches\n", n + ne * ne, bad);
    return bad != 0;
}
