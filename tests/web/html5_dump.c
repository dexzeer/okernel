// Host tool: parse an HTML file with the web engine's HTML5 parser and print
// the tree in html5lib test format (see tests/web/html5_diff.py).
//   html5_dump <file> [charset]
#include <stdio.h>
#include <stdlib.h>
#include "web/wdom.h"

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file [charset]\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); return 2; }
    fclose(f);
    struct wdom* d = whtml_parse(buf, (int)n, argc > 2 ? argv[2] : NULL);
    if (!d) { fprintf(stderr, "parse failed\n"); return 1; }
    int len;
    char* out = wdom_dump(d, &len);
    fwrite(out, 1, len, stdout);
    fprintf(stderr, "nodes=%d attrs=%d text=%u atoms=%d quirks=%d charset=%s title=[%s]\n",
            d->nn, d->na, d->tlen, d->atoms.count, d->quirks, d->charset, d->title);
    free(out);
    wdom_free(d);
    free(buf);
    return 0;
}
