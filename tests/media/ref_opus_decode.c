// The RFC 6716 reference decoder (libopus built from ~/opus_oracle) over the packets written by probe_webm_opus: raw s16le out.
//   ref_opus_decode packets.bin out.pcm channels
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "opus.h"
#include "opus_multistream.h"
int main(int argc, char** argv) {
    if (argc < 4) return 2;
    int ch = atoi(argv[3]), err;
    OpusMSDecoder* d = 0;
    {   // the OpusHead the probe wrote next to the packets
        char hn[512]; snprintf(hn, sizeof hn, "%s.head", argv[1]);
        FILE* hf = fopen(hn, "rb"); unsigned char h[64] = {0};
        if (!hf || fread(h, 1, 64, hf) < 19) { fprintf(stderr, "no head\n"); return 2; }
        fclose(hf);
        ch = h[9];
        unsigned char map[8] = {0, 1};
        if (h[18]) d = opus_multistream_decoder_create(48000, ch, h[19], h[20], h + 21, &err);
        else d = opus_multistream_decoder_create(48000, ch, 1, ch == 2 ? 1 : 0, map, &err);
        if (!d) { fprintf(stderr, "create failed %d\n", err); return 2; }
    }
    FILE *f = fopen(argv[1], "rb"), *o = fopen(argv[2], "wb");
    static unsigned char buf[8192]; static short pcm[5760 * 8];
    uint32_t n; long packets = 0, errs = 0;
    while (fread(&n, 4, 1, f) == 1 && n < sizeof buf) {
        if (fread(buf, 1, n, f) != n) break;
        int r = opus_multistream_decode(d, buf, (int)n, pcm, 5760, 0);
        packets++;
        if (r < 0) { errs++; continue; }
        fwrite(pcm, 2, (size_t)r * ch, o);
    }
    fprintf(stderr, "ref: %ld packets, %ld errors\n", packets, errs);
    return 0;
}
