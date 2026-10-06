// Decode an image with the web engine and dump "w h\n" + RGBA bytes.
//   imgdump <in> <out.raw> [max_dim]
#include <stdio.h>
#include <stdlib.h>
#include "web/image.h"
#include "corpus.h"

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    int len;
    char* d = read_file(argv[1], &len);
    if (!d) return 2;
    int w, h;
    uint32_t* px;
    if (!wimage_decode((const uint8_t*)d, len, argc > 3 ? atoi(argv[3]) : 0, &w, &h, &px)) { printf("FAIL\n"); return 1; }
    FILE* f = fopen(argv[2], "wb");
    fprintf(f, "%d %d\n", w, h);
    for (int i = 0; i < w * h; i++) {
        unsigned char c[4] = { (unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i], (unsigned char)(px[i] >> 24) };
        fwrite(c, 1, 4, f);
    }
    fclose(f);
    printf("OK %d %d\n", w, h);
    return 0;
}
