// Host test for the AAC-LC decoder.
//
//   test_aac imdct                      the IMDCT against the direct formula of the standard
//   test_aac file.aac file.f32          ADTS stream decoded and compared with ffmpeg's float PCM
//
// aac.c is included so the internals (imdct, windows) can be checked too.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/aac.c"

static uint8_t* slurp(const char* path, long* n) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* p = (uint8_t*)malloc((size_t)*n + 8);
    if (fread(p, 1, (size_t)*n, f) != (size_t)*n) exit(2);
    fclose(f);
    return p;
}

static int test_imdct(void) {
    init_tables();
    int bad = 0;
    for (int variant = 0; variant < 2; variant++) {
        const struct mdct* t = variant ? &g_mdct_short : &g_mdct_long;
        int m = t->m;
        double* x = malloc(m * sizeof(double));
        double* out = malloc(2 * m * sizeof(double));
        double* u = malloc(m * sizeof(double));
        double tre[512], tim[512];
        srand(5 + variant);
        for (int i = 0; i < m; i++) x[i] = (rand() / (double)RAND_MAX - 0.5) * 2000;
        imdct(t, x, out, u, tre, tim);
        double maxerr = 0, maxv = 0;
        for (int n = 0; n < 2 * m; n++) {
            double s = 0;
            for (int k = 0; k < m; k++) s += x[k] * cos(M_PI / m * (n + 0.5 + m / 2.0) * (k + 0.5));
            s /= m;
            double e = fabs(s - out[n]);
            if (e > maxerr) maxerr = e;
            if (fabs(s) > maxv) maxv = fabs(s);
        }
        printf("imdct m=%d: max error %.3g (values up to %.1f)\n", m, maxerr, maxv);
        if (maxerr > 1e-9) bad++;
        free(x); free(out); free(u);
    }
    // windows: Princen-Bradley condition w[n]^2 + w[N-1-n]^2 = 1 for the rise half
    for (int sh = 0; sh < 2; sh++) for (int sz = 0; sz < 2; sz++) {
        int n = sz ? 128 : 1024;
        double worst = 0;
        for (int i = 0; i < n; i++) {
            double v = g_win_rise[sh][sz][i] * g_win_rise[sh][sz][i] + g_win_rise[sh][sz][n - 1 - i] * g_win_rise[sh][sz][n - 1 - i];
            if (fabs(v - 1) > worst) worst = fabs(v - 1);
        }
        printf("window shape=%d %s: PR error %.3g\n", sh, sz ? "short" : "long", worst);
        if (worst > 1e-9) bad++;
    }
    printf("imdct/windows: %s\n", bad ? "FAIL" : "PASS");
    return bad;
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "imdct")) return test_imdct();
    if (argc < 3) { fprintf(stderr, "usage: test_aac imdct | test_aac file.aac file.f32\n"); return 2; }
    long n, nref;
    uint8_t* data = slurp(argv[1], &n);
    float* ref = (float*)slurp(argv[2], &nref);
    struct aac_dec* d = aac_new();
    struct aac_adts h;
    long pos = 0;
    long frames = 0, errors = 0;
    long nsamp = 0;                              // per channel
    int ch = 0;
    double maxdiff = 0, sumsq = 0, refsq = 0;
    long over1 = 0, count = 0;
    int loose = strstr(argv[1], "pns") != 0;      // PNS noise is not normative: compare frame energies only
    long energy_bad = 0;
    int16_t pcm[AAC_FRAME * 2];
    FILE* outf = getenv("AAC_OUT") ? fopen(getenv("AAC_OUT"), "wb") : 0;
    while (pos + 7 <= n && aac_adts_parse(data + pos, (size_t)(n - pos), &h) == 0 && pos + h.frame_len <= n) {
        if (!frames) {
            if (aac_set_adts(d, &h) < 0) { printf("config failed\n"); return 1; }
            ch = aac_out_channels(d);
        }
        int r = aac_decode(d, data + pos + h.header_len, (size_t)(h.frame_len - h.header_len), pcm);
        if (r < 0) {
            printf("  frame %ld: error %d\n", frames, r);
            errors++;
        } else {
            double fe_me = 0, fe_ref = 0;
            for (int i = 0; i < r * ch; i++) {
                long idx = nsamp * ch + i;
                if (idx >= nref / 4) break;
                fe_me += (double)pcm[i] * pcm[i];
                fe_ref += (double)ref[idx] * ref[idx] * 32768.0 * 32768.0;
                double want = ref[idx] * 32768.0, diff = fabs(pcm[i] - want);
                if (want > 32767) diff = fabs((double)pcm[i] - 32767);
                if (want < -32768) diff = fabs((double)pcm[i] + 32768);
                if (diff > maxdiff) maxdiff = diff;
                if (diff > 1.5) { over1++; if (over1 <= 3) printf("  frame %ld sample %d: got %d want %.2f\n", frames, i, pcm[i], want); }
                sumsq += diff * diff; refsq += want * want; count++;
            }
            if (getenv("AAC_FRAMES")) {
                double fm = 0;
                for (int i = 0; i < r * ch; i++) {
                    long idx = nsamp * ch + i;
                    if (idx >= nref / 4) break;
                    double df = fabs(pcm[i] - ref[idx] * 32768.0);
                    if (df > fm) fm = df;
                }
                printf("frame %ld max diff %.2f\n", frames, fm);
            }
            if (fe_ref > 1e10 && (fe_me < 0.9 * fe_ref || fe_me > 1.1 * fe_ref)) energy_bad++;
            if (outf) fwrite(pcm, 2, (size_t)r * ch, outf);
            nsamp += r;
        }
        frames++;
        pos += h.frame_len;
    }
    long refn = nref / 4 / (ch ? ch : 1);
    double rms = count ? sqrt(sumsq / count) : 0;
    int ok = !errors && frames > 0 && nsamp == refn && (loose ? energy_bad == 0 : maxdiff <= 2.0);
#ifdef AAC_COVER
    static const char* names[] = { "long", "start", "short", "stop", "groups>1", "tns", "pulse", "escape", "ms0", "ms1", "ms2" };
    for (int i = 0; i < 11; i++) printf(" %s=%d", names[i], g_cover[i]);
    printf("\n codebooks:");
    for (int i = 0; i < 16; i++) printf(" %d=%d", i, g_cover[16 + i]);
    printf("\n");
#endif
    printf("%s: %ld frames, %ld samples/ch (ffmpeg %ld), %d ch, %d Hz; max diff %.2f LSB, rms diff %.3f, >1.5: %ld: %s\n",
           argv[1], frames, nsamp, refn, ch, aac_sample_rate(d), maxdiff, rms, over1, ok ? "PASS" : "FAIL");
    (void)refsq;
    if (outf) fclose(outf);
    aac_free(d);
    return !ok;
}
