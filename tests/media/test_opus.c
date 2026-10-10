// Host test for the Opus front end: decode an opus_demo .bit file with opus_decode() and
// compare with the RFC 6716 reference decoder's output (.ref, 48 kHz s16le).
//
//   test_opus vec/opus_celt_fb_st.bit vec/opus_celt_fb_st.ref [min_snr_db]
//
// Exit 0 with PASS when the stream decodes to the reference within the SNR bound (and at
// most 1 LSB per sample for the CELT-only streams); SKIP when it uses SILK/hybrid frames
// that are not implemented yet.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/opus.h"

static uint8_t* slurp(const char* path, long* n) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* p = malloc(*n + 1);
    if (fread(p, 1, *n, f) != (size_t)*n) exit(2);
    fclose(f);
    return p;
}

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: test_opus in.bit ref.pcm [min_snr_db]\n"); return 2; }
    double min_snr = argc > 3 ? atof(argv[3]) : 60.0;
    long nb, nr;
    uint8_t* bit = slurp(argv[1], &nb);
    int16_t* ref = (int16_t*)slurp(argv[2], &nr);
    long ref_samples = nr / 2;

    int channels = 1;
    {
        char info[1024];
        snprintf(info, sizeof info, "%s", argv[1]);
        char* dot = strrchr(info, '.');
        if (dot) strcpy(dot, ".info");
        FILE* fi = fopen(info, "r");
        char line[256];
        if (fi && fgets(line, sizeof line, fi)) {
            char* c = strstr(line, "channels ");
            if (c) channels = atoi(c + 9);
        }
        if (fi) fclose(fi);
    }
    struct opus_dec* dec = opus_dec_new(channels);
    static int16_t pcm[5760 * 2];
    long pos = 0, out_pos = 0;
    int packets = 0, unsupported = 0, maxdiff = 0;
    double sig = 0, err = 0;

    while (pos + 8 <= nb) {
        uint32_t len = be32(bit + pos);
        pos += 8;
        if (pos + len > (unsigned long)nb) break;
        int n = opus_decode(dec, bit + pos, (int)len, pcm, 5760);
        if (n == OPUS_ERR_UNSUPPORTED) {
            unsupported++;
            pos += len;
            packets++;
            continue;
        }
        if (n < 0) { printf("packet %d: opus_decode returned %d\n", packets, n); return 1; }
        for (int i = 0; i < n * channels; i++, out_pos++) {
            if (out_pos >= ref_samples) break;
            double r = ref[out_pos];
            int dif = abs((int)pcm[i] - (int)ref[out_pos]);
            if (dif > maxdiff) maxdiff = dif;
            sig += r * r;
            err += (pcm[i] - r) * (pcm[i] - r);
        }
        pos += len;
        packets++;
    }
    if (unsupported) {
        printf("%s: SKIP (%d of %d packets use SILK/hybrid frames, not implemented yet)\n", argv[1], unsupported, packets);
        return 0;
    }
    double snr = err > 0 ? 10 * log10(sig / err) : 200;
    int ok = out_pos == ref_samples && snr >= min_snr && maxdiff <= 1;
    printf("%s: %d packets, %ld/%ld samples, max diff %d, SNR %.1f dB\n%s\n", argv[1], packets, out_pos, ref_samples,
           maxdiff, snr, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
