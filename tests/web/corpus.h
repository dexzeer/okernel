// Host-test helpers: load a corpus page (tests/web/corpus/<name>/) with its
// manifest.tsv (url -> local file) so engine tests can resolve sub-resources
// offline.
#ifndef TESTS_WEB_CORPUS_H
#define TESTS_WEB_CORPUS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
size_t strcspn(const char*, const char*);

struct centry { char url[512]; char file[256]; char type[64]; };
struct corpus {
    char dir[256];
    char page_url[512];
    char page_file[256];
    struct centry e[512];
    int n;
};

static char* read_file(const char* path, int* len) {
    FILE* f = fopen(path, "rb");
    if (!f) { *len = 0; return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = (char*)malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); *len = 0; return 0; }
    fclose(f);
    b[n] = 0;
    *len = (int)n;
    return b;
}

static int corpus_open(struct corpus* c, const char* name) {
    memset(c, 0, sizeof *c);
    snprintf(c->dir, sizeof c->dir, "tests/web/corpus/%s", name);
    char p[512];
    snprintf(p, sizeof p, "%s/manifest.tsv", c->dir);
    FILE* f = fopen(p, "r");
    if (!f) return 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* a = line;
        char* b = strchr(a, '\t');
        if (!b) continue;
        *b++ = 0;
        char* t = strchr(b, '\t');
        if (t) *t++ = 0;
        if (!strcmp(a, "PAGE")) {
            snprintf(c->page_url, sizeof c->page_url, "%s", b);
            snprintf(c->page_file, sizeof c->page_file, "%s", t ? t : "index.html");
            continue;
        }
        if (c->n >= 512) continue;
        snprintf(c->e[c->n].url, sizeof c->e[0].url, "%s", a);
        snprintf(c->e[c->n].file, sizeof c->e[0].file, "%s", b);
        snprintf(c->e[c->n].type, sizeof c->e[0].type, "%s", t ? t : "");
        c->n++;
    }
    fclose(f);
    return 1;
}

// Load the bytes for an absolute URL (NULL when not in the corpus).
static char* corpus_get(const struct corpus* c, const char* url, int* len) {
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->e[i].url, url)) {
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", c->dir, c->e[i].file);
            return read_file(p, len);
        }
    *len = 0;
    return 0;
}

static char* corpus_page(const struct corpus* c, int* len) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", c->dir, c->page_file[0] ? c->page_file : "index.html");
    return read_file(p, len);
}

#endif
