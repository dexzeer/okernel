// Decode the Opus track of a WebM file with our decoder: writes raw s16le stereo (arg 2), optionally the packets (arg 3: u32 len + bytes,
// for tests/media/ref_opus_decode.c, the RFC reference decoder), and prints the packet kinds and the clipping of the result.
//   probe_webm_opus in.webm out.pcm [packets.bin]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../src/media/webm.h"
#include "../../src/media/opus.h"

static struct opus_dec* dec;
static FILE *fo, *fp;
static int atrack = -1, chans = 2;
static long npk, kinds[3], frames_ms10[8], unsupported, bad;
static long nstereo, clipped, total;
static int16_t pcm[5760 * 8];
static int raw_mode;                 // RAW=1: all channels, not the stereo mix
static uint8_t head[64];
static int head_len;

static void on_tracks(void* u, const struct webm_info* info) {
    (void)u;
    for (int i = 0; i < info->ntracks; i++)
        if (info->tracks[i].type == WEBM_TRACK_AUDIO) { atrack = info->tracks[i].number; chans = info->tracks[i].channels; }
    fprintf(stderr, "audio track %d, %d channels\n", atrack, chans);
    for (int i = 0; i < info->ntracks; i++)
        if (info->tracks[i].number == atrack && info->tracks[i].priv_len >= 19) {
            const uint8_t* h = info->tracks[i].priv;
            head_len = info->tracks[i].priv_len > 64 ? 64 : info->tracks[i].priv_len;
            memcpy(head, h, head_len);
            int fam = h[18];
            fprintf(stderr, "OpusHead: channels %d family %d", h[9], fam);
            if (fam) fprintf(stderr, " streams %d coupled %d map", h[19], h[20]);
            for (int k = 0; fam && k < h[9]; k++) fprintf(stderr, " %d", h[21 + k]);
            fprintf(stderr, "\n");
            if (fam) dec = opus_dec_new_ms(h[9], h[19], h[20], h + 21);
            else dec = opus_dec_new(chans > 2 ? 2 : chans);
            if (fam) chans = h[9];
        }
    if (!dec) dec = opus_dec_new(2);
}
static void on_frame(void* u, const struct webm_frame* f) {
    (void)u;
    if (f->track != atrack || !dec) return;
    struct opus_packet p;
    if (opus_parse_packet(f->data, (int)f->len, &p) == 0) {
        npk++; kinds[p.mode]++; nstereo += p.stereo;
        int k = p.frame_ms10 == 25 ? 0 : p.frame_ms10 == 50 ? 1 : p.frame_ms10 == 100 ? 2 : p.frame_ms10 == 200 ? 3 : p.frame_ms10 == 400 ? 4 : 5;
        frames_ms10[k] += p.nframes;
    } else bad++;
    if (fp) { uint32_t n = (uint32_t)f->len; fwrite(&n, 4, 1, fp); fwrite(f->data, 1, f->len, fp); }
    int oc = chans > 2 && !raw_mode ? 2 : chans, n;
    if (raw_mode) n = opus_decode_raw(dec, f->data, (int)f->len, pcm, 5760, &oc);
    else n = opus_decode(dec, f->data, (int)f->len, pcm, 5760);
    if (n < 0) { unsupported++; return; }
    for (int i = 0; i < n * oc; i++) { total++; if (pcm[i] >= 32767 || pcm[i] <= -32768) clipped++; }
    fwrite(pcm, 2, (size_t)n * oc, fo);
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fo = fopen(argv[2], "wb");
    if (argc > 3) fp = fopen(argv[3], "wb");
    raw_mode = getenv("RAW") != 0;
    struct webm_cb cb = { on_tracks, on_frame };
    struct webm_demux* d = webm_new(&cb);
    uint8_t buf[65536];
    size_t n;
    long total_in = 0; int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) { rc = webm_feed(d, buf, n); total_in += (long)n; if (rc < 0) { fprintf(stderr, "webm_feed error %d after %ld bytes\n", rc, total_in); break; } }
    printf("packets %ld (bad %ld)  SILK %ld  hybrid %ld  CELT %ld  stereo packets %ld  unsupported %ld\n", npk, bad, kinds[0], kinds[1], kinds[2], nstereo, unsupported);
    printf("frames by size: 2.5ms %ld  5ms %ld  10ms %ld  20ms %ld  40ms %ld  60ms %ld\n", frames_ms10[0], frames_ms10[1], frames_ms10[2], frames_ms10[3], frames_ms10[4], frames_ms10[5]);
    printf("samples %ld, clipped %ld (%.2f%%)\n", total, clipped, total ? 100.0 * clipped / total : 0.0);
    if (argc > 3 && head_len) { char hn[512]; snprintf(hn, sizeof hn, "%s.head", argv[3]); FILE* hf = fopen(hn, "wb"); fwrite(head, 1, head_len, hf); fclose(hf); }
    return 0;
}
