// Host test for the CELT decoder: decode a CELT-only opus_demo .bit file and compare
// with the RFC 6716 reference decoder's output (.ref, 48 kHz s16le).
//
//   test_celt vec/opus_celt_fb_st.bit vec/opus_celt_fb_st.ref
//
// Checks per packet: the decoder's final range equals the encoder's (stored in the
// file - any mis-decoded symbol breaks it), and over the stream the signal-to-error
// ratio against the reference.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/celt.h"
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
    if (argc < 3) { fprintf(stderr, "usage: test_celt in.bit ref.pcm [min_snr_db]\n"); return 2; }
    double min_snr = argc > 3 ? atof(argv[3]) : 60.0;
    long nb, nr;
    uint8_t* bit = slurp(argv[1], &nb);
    int16_t* ref = (int16_t*)slurp(argv[2], &nr);
    long ref_samples = nr / 2;

    struct celt_dec* dec = 0;
    int channels = 0;
    {   // the output channel count is the one the oracle was run with (a stereo stream may carry mono packets)
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
        if (getenv("CELT_CH")) channels = atoi(getenv("CELT_CH"));     // e.g. a mono decoder on a stereo stream
    }
    long pos = 0, out_pos = 0;
    int packets = 0, range_bad = 0, first_bad = -1;
    double sig = 0, err = 0, maxdiff = 0;
    float pcm[960 * 2];
    int verbose = getenv("CELT_VERBOSE") != 0;

    while (pos + 8 <= nb) {
        uint32_t len = be32(bit + pos), rng = be32(bit + pos + 4);
        pos += 8;
        if (pos + len > (unsigned long)nb) break;
        struct opus_packet pk;
        if (opus_parse_packet(bit + pos, (int)len, &pk) < 0 || pk.mode != OPUS_MODE_CELT) {
            printf("SKIP (packet %d is not CELT-only; covered by the full Opus decoder test)\n", packets);
            return 0;
        }
        if (!dec) {
            if (!channels) channels = pk.stereo ? 2 : 1;
            dec = celt_dec_new(channels);
        }
        int end = pk.bandwidth == OPUS_BW_NB ? 13 : pk.bandwidth == OPUS_BW_WB ? 17 : pk.bandwidth == OPUS_BW_SWB ? 19 : 21;
        double pk_err = 0, pk_sig = 0;
        for (int f = 0; f < pk.nframes; f++) {
            int fs = pk.frame_ms10 * 48000 / 10000;
            int n = celt_decode(dec, pk.frame[f], pk.size[f], 0, pcm, fs, 0, end, pk.stereo ? 2 : 1);
            if (n != fs) {
                fprintf(stderr, "packet %d frame %d: celt_decode returned %d\n", packets, f, n);
                return 1;
            }
            for (int i = 0; i < n * channels; i++) {
                double v = pcm[i];
                long q = lrint(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
                if (out_pos < ref_samples) {
                    double r = ref[out_pos];
                    sig += r * r;
                    err += (q - r) * (q - r);
                    pk_err += (q - r) * (q - r);
                    pk_sig += r * r;
                    if (fabs(q - r) > maxdiff) maxdiff = fabs(q - r);
                }
                out_pos++;
            }
        }
        if (rng && celt_final_range(dec) != rng) {
            if (first_bad < 0) {
                first_bad = packets;
                printf("packet %d: stored final range %08x, decoded %08x (len %u)\n", packets, rng, celt_final_range(dec), len);
            }
            range_bad++;
        }
        if (verbose && pk_err > 0)
            printf("packet %3d cfg %d len %4u: err %.0f sig %.0f (%.1f dB)\n", packets, pk.config, len, pk_err, pk_sig,
                   pk_sig > 0 ? 10 * log10(pk_sig / pk_err) : 0.0);
        packets++;
        pos += len;
    }
    double snr = err > 0 ? 10 * log10(sig / err) : 200;
    printf("%s: %d packets, %ld/%ld samples, final-range mismatches %d%s, max diff %.0f, SNR %.1f dB\n",
           argv[1], packets, out_pos, ref_samples, range_bad,
           first_bad >= 0 ? " (first at packet " : "", maxdiff, snr);
    if (first_bad >= 0) printf("  first mismatch: packet %d\n", first_bad);
    int ok = range_bad == 0 && snr >= min_snr && out_pos == ref_samples;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
