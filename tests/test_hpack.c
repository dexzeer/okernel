// Host test for HPACK: the worked examples of RFC 7541 Appendix C (generated from the RFC text), Huffman round trips,
// encoder -> decoder round trips and hostile-input fuzzing (ASan + UBSan).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define HPACK_HOST 1
#include "../src/net/hpack.c"
#include "../src/net/hpack_tables.c"
#include "hpack_vectors.inc"

static int fails;
#define CHECK(c, m) do { if (c) printf("PASS: %s\n", m); else { printf("FAIL: %s\n", m); fails++; } } while (0)

static char got[8192];
static int collect(void* ctx, const char* n, int nl, const char* v, int vl) {
    (void)ctx;
    size_t o = strlen(got);
    if (o + nl + vl + 8 >= sizeof got) return 1;
    memcpy(got + o, n, (size_t)nl); o += (size_t)nl;
    got[o++] = ':'; got[o++] = ' ';
    memcpy(got + o, v, (size_t)vl); o += (size_t)vl;
    got[o++] = '\n'; got[o] = 0;
    return 0;
}

static int unhex(const char* h, uint8_t* out) {
    int n = 0;
    for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); out[n++] = (uint8_t)v; }
    return n;
}

static uint32_t rs = 12345;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

int main(void) {
    // RFC examples. Groups 3/4 are three requests on one connection (table 4096), 5/6 three responses (table 256).
    struct hpack_dec d;
    int cur_group = -1, ok_all = 1, count = 0;
    for (int i = 0; i < NHV; i++) {
        if (HV[i].group != cur_group || HV[i].group == 2) {
            if (cur_group >= 0) hpack_dec_free(&d);
            hpack_dec_init(&d, 4096);
            if (HV[i].group >= 5) d.limit = d.max_size = 256;
            cur_group = HV[i].group;
        }
        uint8_t blk[1024];
        int n = unhex(HV[i].hex, blk);
        got[0] = 0;
        int r = hpack_decode(&d, blk, (uint32_t)n, collect, 0);
        int ok = r == 0 && !strcmp(got, HV[i].expect);
        if (!ok) { printf("  vector %d (group %d) r=%d got:\n%s  want:\n%s", i, HV[i].group, r, got, HV[i].expect); ok_all = 0; }
        count++;
    }
    hpack_dec_free(&d);
    CHECK(ok_all && count == 16, "all 16 worked examples of RFC 7541 Appendix C decode to the published header lists");

    // dynamic table contents after C.4.2 (two entries, 110 bytes)
    {
        struct hpack_dec e;
        hpack_dec_init(&e, 4096);
        uint8_t b1[64], b2[64];
        int n1 = unhex(HV[7].hex, b1), n2 = unhex(HV[8].hex, b2);
        got[0] = 0; hpack_decode(&e, b1, (uint32_t)n1, collect, 0);
        got[0] = 0; hpack_decode(&e, b2, (uint32_t)n2, collect, 0);
        CHECK(e.count == 2 && e.size == 110, "dynamic table: 2 entries, 110 bytes after the second request");
        hpack_dec_free(&e);
    }

    // C.1 integers
    {
        uint8_t b[8];
        uint32_t c = put_int(b, 8, 0, 5, 10);
        CHECK(c == 1 && b[0] == 0x0a, "C.1.1: 10 with a 5-bit prefix");
        c = put_int(b, 8, 0, 5, 1337);
        CHECK(c == 3 && b[0] == 0x1f && b[1] == 0x9a && b[2] == 0x0a, "C.1.2: 1337 with a 5-bit prefix");
        c = put_int(b, 8, 0, 8, 42);
        CHECK(c == 1 && b[0] == 42, "C.1.3: 42 with an 8-bit prefix");
        uint32_t v;
        CHECK(get_int(b, 1, 8, &v) == 1 && v == 42, "decode 42");
        uint8_t big[] = { 0x1f, 0x9a, 0x0a };
        CHECK(get_int(big, 3, 5, &v) == 3 && v == 1337, "decode 1337");
        uint8_t evil[] = { 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01 };
        CHECK(get_int(evil, 8, 7, &v) == 0, "an absurdly long integer is refused");
        uint8_t cut[] = { 0x1f, 0x9a };
        CHECK(get_int(cut, 2, 5, &v) == 0, "a truncated integer is refused");
    }

    // Huffman: round trip every byte value, random strings, the padding and EOS rules
    {
        uint8_t all[256], enc[1024], dec[1024];
        for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
        uint32_t el = hpack_huff_encode(all, 256, enc, sizeof enc);
        int dl = hpack_huff_decode(enc, el, dec, sizeof dec);
        CHECK(el > 0 && dl == 256 && !memcmp(all, dec, 256), "Huffman round trip of all 256 byte values");
        int bad = 0;
        for (int t = 0; t < 2000; t++) {
            uint8_t s[200];
            int n = (int)(rnd() % 200);
            for (int i = 0; i < n; i++) s[i] = (uint8_t)rnd();
            uint32_t e = hpack_huff_encode(s, (uint32_t)n, enc, sizeof enc);
            int d2 = hpack_huff_decode(enc, e, dec, sizeof dec);
            if (n && (d2 != n || memcmp(s, dec, (size_t)n))) bad++;
            if (!n && d2 != 0) bad++;
        }
        CHECK(bad == 0, "2000 random strings round trip");
        uint8_t www[] = { 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff };
        int n = hpack_huff_decode(www, sizeof www, dec, sizeof dec);
        CHECK(n == 15 && !memcmp(dec, "www.example.com", 15), "www.example.com from the RFC");
        uint8_t pad0[] = { 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xfe };
        CHECK(hpack_huff_decode(pad0, sizeof pad0, dec, sizeof dec) < 0, "padding that is not all ones is refused");
        uint8_t eos[] = { 0xff, 0xff, 0xff, 0xff };
        CHECK(hpack_huff_decode(eos, sizeof eos, dec, sizeof dec) < 0, "EOS inside a string is refused");
        uint8_t longpad[] = { 0xff };
        CHECK(hpack_huff_decode(longpad, 1, dec, sizeof dec) < 0, "more than 7 bits of padding is refused");
    }

    // encoder -> decoder round trip, including never-indexed and a long value
    {
        struct hpack_dec e;
        hpack_dec_init(&e, 4096);
        uint8_t blk[8192];
        uint32_t o = 0;
        static char longv[3000];
        memset(longv, 'x', sizeof longv - 1);
        const char* f[][2] = { { ":method", "GET" }, { ":scheme", "https" }, { ":path", "/a?b=c" }, { ":authority", "x.test" },
                               { "user-agent", "okernel/0.4" }, { "accept", "*/*" }, { "accept-encoding", "gzip" },
                               { "x-custom", "v" }, { "cookie", "a=b; c=d" }, { "x-long", longv }, { ":method", "POST" },
                               { "content-length", "12" } };
        got[0] = 0;
        char want[8192] = "";
        for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) {
            uint32_t c = hpack_encode_field(blk + o, sizeof blk - o, f[i][0], (uint32_t)strlen(f[i][0]), f[i][1], (uint32_t)strlen(f[i][1]),
                                            !strcmp(f[i][0], "cookie"));
            if (!c) { printf("encode failed at %u\n", i); fails++; break; }
            o += c;
            strcat(want, f[i][0]); strcat(want, ": "); strcat(want, f[i][1]); strcat(want, "\n");
        }
        int r = hpack_decode(&e, blk, o, collect, 0);
        CHECK(r == 0 && !strcmp(got, want), "encoded header list decodes to itself");
        CHECK(blk[0] == 0x82 && blk[1] == 0x87, ":method GET and :scheme https are single bytes");
        CHECK(e.count == 0, "the encoder never touches the peer's dynamic table");
        hpack_dec_free(&e);
    }

    // dynamic table rules: size update beyond our limit, eviction, entry larger than the table
    {
        struct hpack_dec e;
        hpack_dec_init(&e, 100);
        uint8_t up[] = { 0x3f, 0xe1, 0x1f };                         // size update to 4096
        CHECK(hpack_decode(&e, up, sizeof up, collect, 0) == -1, "a table size update above the announced limit is an error");
        uint8_t ins[] = { 0x40, 0x03, 'a', 'b', 'c', 0x03, 'd', 'e', 'f' };          // literal with indexing: abc: def (38 bytes)
        got[0] = 0;
        hpack_decode(&e, ins, sizeof ins, collect, 0);
        hpack_decode(&e, ins, sizeof ins, collect, 0);
        CHECK(e.count == 2 && e.size == 76, "two entries fit in 100 bytes");
        hpack_decode(&e, ins, sizeof ins, collect, 0);
        CHECK(e.count == 2 && e.size == 76, "a third evicts the oldest");
        uint8_t shrink[] = { 0x20 };                                  // size update to 0
        hpack_decode(&e, shrink, sizeof shrink, collect, 0);
        CHECK(e.count == 0 && e.size == 0, "size update to 0 empties the table");
        uint8_t bigone[40];
        bigone[0] = 0x40; bigone[1] = 0x03; memcpy(bigone + 2, "xyz", 3); bigone[5] = 31; memset(bigone + 6, 'v', 31);   // 3+31+32 = 66
        uint8_t grow[] = { 0x3f, 0x25 };                              // size update to 68
        hpack_decode(&e, grow, sizeof grow, collect, 0);
        hpack_decode(&e, bigone, 37, collect, 0);
        CHECK(e.count == 1, "an entry that fits is added");
        uint8_t small[] = { 0x2a };                                   // size update to 10
        hpack_decode(&e, small, sizeof small, collect, 0);
        hpack_decode(&e, bigone, 37, collect, 0);
        CHECK(e.count == 0, "an entry larger than the whole table empties it and is not added");
        uint8_t badidx[] = { 0xff, 0x00 };                            // index 127 with an empty dynamic table
        CHECK(hpack_decode(&e, badidx, sizeof badidx, collect, 0) == -1, "an index past the tables is an error");
        uint8_t zero[] = { 0x80 };                                    // index 0
        CHECK(hpack_decode(&e, zero, sizeof zero, collect, 0) == -1, "index 0 is an error");
        hpack_dec_free(&e);
    }

    // hostile input: random blocks and mutated real ones never crash (ASan/UBSan do the judging)
    {
        struct hpack_dec e;
        hpack_dec_init(&e, 4096);
        for (int t = 0; t < 20000; t++) {
            uint8_t b[300];
            int n = (int)(rnd() % 300);
            if (t % 2) {
                int vi = (int)(rnd() % NHV);
                n = unhex(HV[vi].hex, b);
                for (int k = 0; k < 3 && n; k++) b[rnd() % (uint32_t)n] = (uint8_t)rnd();
            } else {
                for (int i = 0; i < n; i++) b[i] = (uint8_t)rnd();
            }
            got[0] = 0;
            hpack_decode(&e, b, (uint32_t)n, collect, 0);
        }
        CHECK(e.size <= 4096, "the dynamic table stays within its limit under 20000 hostile blocks");
        hpack_dec_free(&e);
    }
    printf(fails ? "HPACK TESTS FAIL\n" : "HPACK TESTS PASS\n");
    return fails != 0;
}
