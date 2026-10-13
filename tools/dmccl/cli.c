// dmccl command line: runs a script on the host. `dmccl file.dmccl [--realtime] [--max-seconds N]`
// Time is virtual unless --realtime (so `waitfor:60s` finishes at once); the script ends when nothing is left to run.
#include "../../src/dmccl/dm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void say(void* user, const char* line) { (void)user; puts(line); fflush(stdout); }

int main(int argc, char** argv) {
    const char* path = 0;
    int realtime = 0;
    double max_seconds = 3600;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--realtime")) realtime = 1;
        else if (!strcmp(argv[i], "--max-seconds") && i + 1 < argc) max_seconds = atof(argv[++i]);
        else path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: dmccl file.dmccl [--realtime] [--max-seconds N]\n"); return 2; }
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot read '%s'\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* src = (char*)malloc((size_t)n + 1);
    if (fread(src, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "error: short read\n"); return 1; }
    fclose(f);
    char err[256];
    dm_program* prog = dm_compile(src, (size_t)n, err, sizeof err);
    free(src);
    if (!prog) { fprintf(stderr, "compile error: %s\n", err); return 1; }
    struct dm_host host;
    memset(&host, 0, sizeof host);
    host.print = say;
    dm_vm* vm = dm_vm_new(prog, &host);
    dm_register_basics(vm);
    if (dm_link(vm, err, sizeof err)) { fprintf(stderr, "link error: %s\n", err); return 1; }
    dm_start(vm);
    long steps = 0;
    while (!dm_idle(vm) && steps * 10 < (long)(max_seconds * 1000)) {
        dm_step(vm, 10);
        steps++;
        if (realtime) { struct timespec ts = { 0, 10 * 1000 * 1000 }; nanosleep(&ts, 0); }
    }
    dm_vm_free(vm);
    dm_program_free(prog);
    return 0;
}
