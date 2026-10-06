// Host renderer: corpus page -> PPM through the full web engine.
//   render <corpus-name|file.html> [width] [height] [out.ppm] [max_page_height]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "web/wdoc.h"
#include "web/wdom.h"
#include "corpus.h"
char* strstr(const char*, const char*);

// overridden by src/web/image.c when linked
__attribute__((weak)) int wimage_decode(const uint8_t* data, int len, int max_dim, int* w, int* h, uint32_t** px) {
    (void)data; (void)len; (void)max_dim; (void)w; (void)h; (void)px;
    return 0;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void save_ppm(const char* path, struct wsurf* s) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    unsigned char* row = malloc(s->w * 3);
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            uint32_t p = s->px[y * s->stride + x];
            row[x * 3] = (unsigned char)(p >> 16);
            row[x * 3 + 1] = (unsigned char)(p >> 8);
            row[x * 3 + 2] = (unsigned char)p;
        }
        fwrite(row, 1, s->w * 3, f);
    }
    free(row);
    fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s corpus|file.html [w] [h] [out.ppm] [maxh]\n", argv[0]); return 2; }
    int vw = argc > 2 ? atoi(argv[2]) : 1280;
    int vh = argc > 3 ? atoi(argv[3]) : 800;
    const char* out = argc > 4 ? argv[4] : "tests/web/out/render.ppm";
    int maxh = argc > 5 ? atoi(argv[5]) : vh;
    struct corpus c;
    char* html;
    int len;
    const char* url;
    int have_corpus = 0;
    if (strstr(argv[1], ".html")) {
        html = read_file(argv[1], &len);
        url = "file:///local/test.html";
    } else {
        if (!corpus_open(&c, argv[1])) { fprintf(stderr, "no corpus %s\n", argv[1]); return 1; }
        html = corpus_page(&c, &len);
        url = c.page_url;
        have_corpus = 1;
    }
    if (!html) return 1;
    double t0 = now_ms();
    struct wdoc* d = wdoc_new();
    wdoc_set_viewport(d, vw, vh);
    wdoc_load(d, url, html, len, 0);
    double t1 = now_ms();
    char rurl[1024];
    int id, fetched = 0, missing = 0;
    while ((id = wdoc_next_fetch(d, rurl, sizeof rurl)) >= 0) {
        int bl = 0;
        char* bytes = have_corpus ? corpus_get(&c, rurl, &bl) : 0;
        if (bytes) { wdoc_fetch_done(d, id, bytes, bl, ""); fetched++; free(bytes); }
        else { wdoc_fetch_done(d, id, 0, -1, ""); missing++; }
    }
    double t2 = now_ms();
    wdoc_update(d);
    double t3 = now_ms();
    int dh = wdoc_height(d);
    int ph = dh < maxh ? dh : maxh;
    if (ph < vh) ph = vh;
    struct wsurf s;
    s.w = vw; s.h = ph; s.stride = vw;
    s.px = (uint32_t*)malloc(sizeof(uint32_t) * vw * ph);
    ws_reset_clip(&s);
    wdoc_paint(d, &s, 0);
    double t4 = now_ms();
    char stats[256];
    wdoc_stats(d, stats, sizeof stats);
    printf("%s: %s docH=%d parse=%.1fms res=%.1fms(%d ok,%d missing) layout=%.1fms paint=%.1fms title=[%s]\n",
           argv[1], stats, dh, t1 - t0, t2 - t1, fetched, missing, t3 - t2, t4 - t3, wdoc_title(d));
    if (system("mkdir -p tests/web/out") != 0) {}
    save_ppm(out, &s);
    free(s.px);
    wdoc_free(d);
    free(html);
    return 0;
}
