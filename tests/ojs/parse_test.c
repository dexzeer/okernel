// Parser-only host test: parse files given on stdin as "<mode> <path>"
// lines (mode: s = sloppy script, S = strict script, m = module) and print
// "OK" or "ERR <message>" per line. Used by tests/ojs/run_parse262.py.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/ojs/parse.h"

void* ojs_sys_malloc(size_t n) { return malloc(n); }
void* ojs_sys_realloc(void* p, size_t n) { return realloc(p, n); }
void ojs_sys_free(void* p) { free(p); }

// ---- stubs for engine parts the parser does not use
void shape_trace(ojs* J, struct shape* s) { (void)J; (void)s; }
void obj_trace(ojs* J, struct obj* o) { (void)J; (void)o; }
void ftempl_trace(ojs* J, struct gch* g) { (void)J; (void)g; }
void upval_trace(ojs* J, struct gch* g) { (void)J; (void)g; }
void maptab_trace(ojs* J, struct gch* g) { (void)J; (void)g; }
void module_trace(ojs* J, struct gch* g) { (void)J; (void)g; }
void frame_trace(ojs* J, struct gch* g) { (void)J; (void)g; }
void gc_finalize(ojs* J, struct gch* g) { (void)J; (void)g; }
void vm_mark_roots(ojs* J) { (void)J; }
void module_mark_roots(ojs* J) { (void)J; }
int weak_process_ephemerons(ojs* J) { (void)J; return 0; }
void finreg_process(ojs* J) { (void)J; }
void weak_clear_dead(ojs* J) { (void)J; }
jv to_primitive(ojs* J, jv v, int hint) { (void)J; (void)hint; return v; }
struct str* to_str(ojs* J, jv v) { (void)J; return jv_is_str(v) ? jv_str(v) : 0; }
static char last_error[512];
jv err_new(ojs* J, int ne, jv message) { (void)J; (void)ne; return message; }
int ojs_eval_env_has_private(struct ojs_eval_env* e, struct str* name) { (void)e; (void)name; return 0; }
int regexp_check_syntax(ojs* J, struct str* body, struct str* flags, char* err, int errcap) {
    (void)J; (void)body; (void)flags; (void)err; (void)errcap;
    return 0;   // regex syntax is checked by re.c (not linked in this test)
}
int atoms_init(ojs* J);

int main(void) {
    static ojs JJ;
    ojs* J = &JJ;
    gc_init(J);
    J->gc_disabled = 1;
    if (atoms_init(J) < 0) { printf("init failed\n"); return 2; }
    char line[4096];
    static char buf[4 << 20];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) continue;
        char mode = line[0];
        const char* path = line + 2;
        FILE* f = fopen(path, "rb");
        if (!f) { printf("ERR cannot open\n"); fflush(stdout); continue; }
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        struct str* src = str_from_utf8(J, buf, n);
        struct parse_opts o;
        memset(&o, 0, sizeof o);
        o.kind = mode == 'm' ? PARSE_MODULE : PARSE_SCRIPT;
        o.strict = mode == 'S';
        o.filename = path;
        struct parser P;
        int r = parse_program(J, src, &o, &P);
        if (r == 0) printf("OK\n");
        else {
            jv e = take_exc(J);
            char msg[400] = "?";
            if (jv_is_str(e)) str_to_utf8_buf(jv_str(e), msg, sizeof msg);
            for (char* p = msg; *p; p++) if (*p == '\n') *p = ' ';
            printf("ERR %s\n", msg);
        }
        parse_free(&P);
        fflush(stdout);
        // reclaim memory between files: the heap only grows in this test
        (void)last_error;
    }
    return 0;
}
