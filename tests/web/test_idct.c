// IDCT accuracy vs the T.81 definition evaluated in double precision.
//   gcc -m32 -O2 -Isrc -Itests/web -o build-host/t_idct tests/web/test_idct.c -lm
// Includes image.c to reach the static idct_block(). Random blocks at
// several coefficient magnitudes (sparse like real JPEGs, and dense); the
// reference is rounded and clamped like the decoder. Expect |err| <= 1.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "web/image.c"

static void ref_idct(uint8_t* out, const int16_t* S) {
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            double s = 0;
            for (int v = 0; v < 8; v++)
                for (int u = 0; u < 8; u++) {
                    double cu = u ? 1 : 1 / sqrt(2), cv = v ? 1 : 1 / sqrt(2);
                    s += cu * cv * S[v * 8 + u] * cos((2 * x + 1) * u * M_PI / 16) *
                         cos((2 * y + 1) * v * M_PI / 16);
                }
            double r = floor(s / 4 + 128 + 0.5);
            out[y * 8 + x] = r < 0 ? 0 : r > 255 ? 255 : (uint8_t)r;
        }
}

int main(void) {
    srand(12345);
    int worst = 0, off1 = 0, n = 0;
    static const int mags[] = {16, 64, 256, 1024, 2047, 1024, 2047};
    for (int m = 0; m < 7; m++)
        for (int it = 0; it < 4000; it++) {
            int16_t S[64] = {0};
            int dense = it & 1;
            for (int k = 0; k < 64; k++) {
                if (!dense && k && rand() % 6) continue;   // sparse: mostly zero AC
                int lim = (k && m < 5) ? mags[m] / (1 + k / 8) : mags[m];   // m >= 5: flat spectrum (q100)
                S[k] = (int16_t)(rand() % (2 * lim + 1) - lim);
            }
            uint8_t a[64], b[64];
            idct_block(a, 8, S);
            ref_idct(b, S);
            for (int i = 0; i < 64; i++) {
                int d = abs(a[i] - b[i]);
                if (d > worst) worst = d;
                off1 += d == 1;
                n++;
            }
        }
    printf("idct: %d samples, max err %d, off-by-one %.3f%%\n", n, worst, 100.0 * off1 / n);
    printf("IDCT %s\n", worst <= 1 ? "PASS" : "FAIL");
    return worst > 1;
}
