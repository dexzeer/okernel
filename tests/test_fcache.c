// Host test for the HTTP response cache: freshness rules, date arithmetic, key lookup, eviction.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define FCACHE_HOST 1
#include "../src/net/fcache.c"

static int fails;
#define CHECK(c, m) do { if (c) printf("PASS: %s\n", m); else { printf("FAIL: %s\n", m); fails++; } } while (0)

static int fresh(const char* hdrs, uint32_t* t, int prefetch) {
    static char buf[2048];
    snprintf(buf, sizeof buf, "HTTP/1.1 200 OK\r\n%s\r\nContent-Length: 2\r\n\r\nhi", hdrs);
    return fcache_freshness(buf, (uint32_t)strlen(buf), t, prefetch);
}

int main(void) {
    uint32_t t = 0;
    CHECK(fresh("Cache-Control: max-age=600", &t, 0) && t == 60000, "max-age=600 is 600 s");
    CHECK(fresh("Cache-Control: public, max-age=60, immutable", &t, 0) && t == 8640000, "immutable counts as a day");
    CHECK(fresh("Cache-Control: max-age=100\r\nAge: 40", &t, 0) && t == 6000, "Age is subtracted");
    CHECK(!fresh("Cache-Control: no-store, max-age=600", &t, 0), "no-store is never stored");
    CHECK(!fresh("Cache-Control: no-store", &t, 1), "not even when prefetched");
    CHECK(!fresh("Cache-Control: no-cache, max-age=600", &t, 0), "no-cache is not stored");
    CHECK(fresh("Cache-Control: no-cache", &t, 1) && t == 3000, "a prefetched no-cache response is kept for 30 s");
    CHECK(!fresh("Cache-Control: max-age=600\r\nSet-Cookie: a=b", &t, 0), "Set-Cookie responses are not stored");
    CHECK(!fresh("Cache-Control: max-age=600\r\nVary: Cookie", &t, 0), "Vary: Cookie is not stored");
    CHECK(fresh("Cache-Control: max-age=600\r\nVary: Accept-Encoding, Origin", &t, 0), "Vary: Accept-Encoding, Origin is fine");
    CHECK(!fresh("Cache-Control: max-age=600\r\nVary: *", &t, 0), "Vary: * is not stored");
    CHECK(fresh("Date: Wed, 21 Oct 2015 07:28:00 GMT\r\nExpires: Wed, 21 Oct 2015 07:38:00 GMT", &t, 0) && t == 60000, "Expires minus Date");
    CHECK(!fresh("Date: Wed, 21 Oct 2015 07:28:00 GMT\r\nExpires: 0", &t, 0), "Expires: 0 is stale");
    CHECK(fresh("Date: Wed, 21 Oct 2015 07:28:00 GMT\r\nLast-Modified: Tue, 20 Oct 2015 07:28:00 GMT", &t, 0) && t == 864000, "10% of the age since Last-Modified");
    CHECK(!fresh("X-Nothing: 1", &t, 0), "no freshness information: not stored");
    CHECK(fresh("X-Nothing: 1", &t, 1) && t == 3000, "...but a prefetch keeps it 30 s");
    CHECK(fresh("Cache-Control: max-age=99999999", &t, 0) && t == 8640000, "lifetimes are capped at a day");
    {
        static const char r404[] = "HTTP/1.1 404 Not Found\r\nCache-Control: max-age=600\r\n\r\n";
        CHECK(!fcache_freshness(r404, sizeof r404 - 1, &t, 0), "only 200 is stored");
    }
    // store / lookup / expiry / replace / clear
    struct fcache_key a, b;
    fcache_key_init(&a); fcache_key_add(&a, "https", 5); fcache_key_add(&a, "x.test/a.css", 12);
    fcache_key_init(&b); fcache_key_add(&b, "https", 5); fcache_key_add(&b, "x.test/b.css", 12);
    static const char R[] = "HTTP/1.1 200 OK\r\nCache-Control: max-age=10\r\nContent-Length: 2\r\n\r\nhi";
    const char* d; uint32_t n;
    CHECK(!fcache_lookup(&a, 100, &d, &n), "miss on an empty cache");
    CHECK(fcache_store(&a, R, sizeof R - 1, 100, 0), "store");
    CHECK(fcache_lookup(&a, 150, &d, &n) && n == sizeof R - 1 && !memcmp(d, R, n), "hit returns the same bytes");
    CHECK(!fcache_lookup(&b, 150, &d, &n), "another key misses");
    CHECK(fcache_lookup(&a, 1099, &d, &n), "still fresh just before 10 s");
    CHECK(!fcache_lookup(&a, 1100, &d, &n), "gone after 10 s");
    CHECK(fcache_store(&a, R, sizeof R - 1, 2000, 0) && fcache_store(&a, R, sizeof R - 1, 2000, 0), "replacing the same key works");
    int ent; uint32_t bytes;
    fcache_stats(&ent, &bytes, 0, 0);
    CHECK(ent == 1 && bytes == sizeof R - 1, "one entry, its bytes counted once");
    // eviction: a lot of 1 MB entries stay under the cap
    {
        static char big[1 << 20];
        memset(big, 'x', sizeof big);
        const char* hd = "HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\n\r\n";
        memcpy(big, hd, strlen(hd));
        for (int i = 0; i < 100; i++) {
            struct fcache_key k; fcache_key_init(&k); char nm[16]; snprintf(nm, sizeof nm, "k%d", i); fcache_key_add(&k, nm, (uint32_t)strlen(nm));
            fcache_store(&k, big, sizeof big, 3000, 0);
        }
        fcache_stats(&ent, &bytes, 0, 0);
        CHECK(bytes <= (32u << 20) && ent > 10, "total size stays under the cap");
    }
    fcache_clear();
    fcache_stats(&ent, &bytes, 0, 0);
    CHECK(ent == 0 && bytes == 0, "clear empties it");
    printf(fails ? "FCACHE TESTS FAIL\n" : "FCACHE TESTS PASS\n");
    return fails != 0;
}
