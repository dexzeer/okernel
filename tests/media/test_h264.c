// H.264 decoder test: decode an Annex B stream (or the video track of an MP4 via ffmpeg-made .h264
// files) and compare every frame, in display order, with a reference YUV from ffmpeg.
//   test_h264 stream.h264 reference.yuv
// Prints "PASS" or the first mismatch (frame, plane, x, y).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/h264.h"

struct rec { int idr_epoch, poc; int w, h; uint8_t* y; uint8_t* u; uint8_t* v; };
static struct rec* recs; static int nrec, caprec, epoch;

static void on_pic(void* user, struct h264_pic* p) {
    (void)user;
    if (p->is_idr) epoch++;
    if (nrec == caprec) { caprec = caprec ? caprec * 2 : 64; recs = (struct rec*)realloc(recs, (size_t)caprec * sizeof *recs); }
    struct rec* r = &recs[nrec++];
    r->idr_epoch = epoch; r->poc = p->poc; r->w = p->width; r->h = p->height;
    r->y = (uint8_t*)malloc((size_t)p->width * p->height);
    r->u = (uint8_t*)malloc((size_t)p->chroma_width * p->chroma_height);
    r->v = (uint8_t*)malloc((size_t)p->chroma_width * p->chroma_height);
    for (int y = 0; y < p->height; y++) memcpy(r->y + (size_t)y * p->width, p->plane[0] + (size_t)y * p->stride[0], (size_t)p->width);
    for (int y = 0; y < p->chroma_height; y++) {
        memcpy(r->u + (size_t)y * p->chroma_width, p->plane[1] + (size_t)y * p->stride[1], (size_t)p->chroma_width);
        memcpy(r->v + (size_t)y * p->chroma_width, p->plane[2] + (size_t)y * p->stride[2], (size_t)p->chroma_width);
    }
    h264_pic_unref(p);
}

static int cmp_rec(const void* a, const void* b) {
    const struct rec* x = (const struct rec*)a; const struct rec* y = (const struct rec*)b;
    if (x->idr_epoch != y->idr_epoch) return x->idr_epoch - y->idr_epoch;
    return x->poc - y->poc;
}

static uint8_t* slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc((size_t)l + 1);
    if (fread(b, 1, (size_t)l, f) != (size_t)l) exit(2);
    fclose(f);
    *n = (size_t)l;
    return b;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s stream.h264 reference.yuv\n", argv[0]); return 2; }
    size_t n, rn;
    uint8_t* es = slurp(argv[1], &n);
    uint8_t* ref = slurp(argv[2], &rn);
    struct h264_dec* d = h264_new();
    h264_set_annexb(d);
    int rc = h264_decode(d, es, n, 0, on_pic, 0);
    if (rc < 0) printf("decode error %d after %d pictures; ", rc, nrec);
    qsort(recs, (size_t)nrec, sizeof *recs, cmp_rec);
    if (nrec == 0) { printf("FAIL no pictures\n"); return 1; }
    size_t fsz = (size_t)recs[0].w * recs[0].h * 3 / 2;
    if (rn != fsz * (size_t)nrec) { printf("FAIL %d frames decoded (%zu bytes) but the reference holds %zu bytes (%.2f frames)\n", nrec, fsz * (size_t)nrec, rn, (double)rn / (double)fsz); }
    int nframes = (int)(rn / fsz) < nrec ? (int)(rn / fsz) : nrec;
    int bad = 0;
    for (int i = 0; i < nframes && !bad; i++) {
        const uint8_t* r = ref + fsz * (size_t)i;
        struct rec* c = &recs[i];
        size_t ys = (size_t)c->w * c->h, cs = ys / 4;
        for (size_t k = 0; k < ys; k++) if (c->y[k] != r[k]) { printf("FAIL frame %d luma x=%zu y=%zu got %d want %d\n", i, k % (size_t)c->w, k / (size_t)c->w, c->y[k], r[k]); bad = 1; break; }
        if (bad) break;
        for (size_t k = 0; k < cs; k++) if (c->u[k] != r[ys + k]) { printf("FAIL frame %d Cb x=%zu y=%zu got %d want %d\n", i, k % (size_t)(c->w / 2), k / (size_t)(c->w / 2), c->u[k], r[ys + k]); bad = 1; break; }
        if (bad) break;
        for (size_t k = 0; k < cs; k++) if (c->v[k] != r[ys + cs + k]) { printf("FAIL frame %d Cr x=%zu y=%zu got %d want %d\n", i, k % (size_t)(c->w / 2), k / (size_t)(c->w / 2), c->v[k], r[ys + cs + k]); bad = 1; break; }
    }
    if (!bad && rn == fsz * (size_t)nrec) printf("PASS %d frames %dx%d\n", nrec, recs[0].w, recs[0].h);
    h264_free(d);
    return bad || rn != fsz * (size_t)nrec;
}
