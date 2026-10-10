// Host test for the pure Intel-graphics helpers in src/i915.c: device id
// classification and register/PTE decoding. The id list below is independent of
// the implementation (spot checks per platform across all generations).
#include <stdio.h>
#include <string.h>
#include "../src/i915.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct id { uint16_t id; uint8_t gen; };

int main(void) {
    static const struct id ids[] = {
        { 0x2582, 3 }, { 0x27A2, 3 }, { 0x2972, 4 }, { 0x2A42, 4 }, { 0x2E22, 4 }, { 0xA011, 3 },
        { 0x0042, 5 }, { 0x0046, 5 },
        { 0x0102, 6 }, { 0x0116, 6 }, { 0x0126, 6 },
        { 0x0152, 7 }, { 0x0162, 7 }, { 0x0166, 7 }, { 0x0F31, 7 },
        { 0x0412, 7 }, { 0x0A16, 7 }, { 0x0D22, 7 }, { 0x0C06, 7 },
        { 0x1616, 8 }, { 0x1626, 8 }, { 0x162B, 8 }, { 0x22B1, 8 },
        { 0x1912, 9 }, { 0x191B, 9 }, { 0x1926, 9 }, { 0x5912, 9 }, { 0x591B, 9 }, { 0x3E92, 9 }, { 0x3E9B, 9 },
        { 0x9B41, 9 }, { 0x9BC8, 9 }, { 0x3185, 9 }, { 0x1A84, 9 }, { 0x5A85, 9 },
        { 0x8A52, 11 }, { 0x8A56, 11 }, { 0x4541, 11 },
        { 0x9A49, 12 }, { 0x9A40, 12 }, { 0x4C8A, 12 }, { 0x4680, 12 }, { 0x46A6, 12 }, { 0xA780, 12 }, { 0x4905, 12 },
    };
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        struct i915_gen g = i915_classify(ids[i].id);
        CHECK(g.gen == ids[i].gen, "id %04x: gen %d, want %d (%s)", ids[i].id, g.gen, ids[i].gen, g.name);
    }
    CHECK(i915_classify(0x1234).gen == 0, "unknown id must stay unknown");
    CHECK(i915_display_base(0x22B1) == 0x180000 && i915_display_base(0x0F31) == 0x180000 && i915_display_base(0x1912) == 0 &&
          i915_display_base(0x0166) == 0, "Valleyview/Cherryview display registers sit at BAR0 + 0x180000");
    CHECK(strstr(i915_classify(0x1912).name, "Skylake") != 0, "name");
    CHECK(strstr(i915_classify(0x9A49).name, "Tiger") != 0, "name");

    // plane control decode
    CHECK(i915_plane_enabled(9, 0x84000000u) == 1 && i915_plane_enabled(9, 0x04000000u) == 0, "enable bit");
    CHECK(strcmp(i915_plane_format(9, 4u << 24), "XRGB 8888") == 0, "gen9 format");
    CHECK(strcmp(i915_plane_format(7, 6u << 26), "XRGB 8888") == 0, "gen7 format");
    CHECK(strcmp(i915_plane_format(7, 2u << 26), "8bpp indexed") == 0, "gen7 8bpp");
    CHECK(i915_plane_tiling(9, 0) == 0 && i915_plane_tiling(9, 1u << 10) == 1 && i915_plane_tiling(9, 4u << 10) == 2, "gen9 tiling");
    CHECK(i915_plane_tiling(7, 0) == 0 && i915_plane_tiling(7, 1u << 10) == 1, "gen7 tiling");

    // PTEs
    CHECK(i915_pte_phys(8, 0x12345003u, 1) == 0x112345000ull, "gen8 pte");
    CHECK(i915_pte_phys(8, 0x12345003u, 0x80) == 0x12345000ull, "gen8 pte ignores bit 39+");
    CHECK(i915_pte_phys(7, 0x12345013u, 0) == 0x112345000ull, "gen7 pte high bits");
    CHECK(i915_pte_phys(5, 0x12345001u, 0) == 0x12345000ull, "gen5 pte");
    CHECK(i915_pte_valid(9, 3) == 1 && i915_pte_valid(9, 2) == 0, "valid bit");

    printf(fails ? "FAILED %d\n" : "ALL OK\n", fails);
    return fails != 0;
}
