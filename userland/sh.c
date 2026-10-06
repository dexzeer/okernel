// sh.c — tiny ring-3 shell (/bin/sh). Runs as init's child on a terminal:
// read line (fd 0) -> parse -> fork/exec/wait -> prompt (fd 1). Builtins:
// exit (leave), help. Everything else forks + execs (PATH: exact, /bin/).

#include "usys.h"

static unsigned kstrlen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static int kstreq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void puts1(const char *s) {
    usys_write(1, s, kstrlen(s));
}

void _start(void) {
    puts1("user sh ready\n");
    char line[256];
    for (;;) {
        puts1("u> ");
        for (unsigned k = 0; k < sizeof(line); k++) line[k] = 0;
        int n = usys_read(0, line, sizeof(line) - 1);
        if (n < 0) continue; // read error — reprompt
        if (n == 0) {
            // No line yet (kbd queue empty): yield so the tick can run the
            // kernel idle loop instead of spinning 100% in ring 3.
            usys_yield();
            continue;
        }
        // strip trailing newline/CR
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) n--;
        line[n] = 0;
        if (n == 0) continue;
        if (kstreq(line, "exit")) {
            puts1("bye\n");
            usys_exit(0);
        }
        if (kstreq(line, "help")) {
            puts1("user sh: type a program path, exit to leave\n");
            continue;
        }
        int child = usys_fork();
        if (child < 0) {
            puts1("fork failed\n");
            continue;
        }
        if (child == 0) {
            // NOTE: NO stack locals here (bisected 2026-09-08: EBX garbage
            // — the child's entry ESP lands BELOW the parent's trap-time
            // ESP (stack moved between fork-trap and child-entry), so the
            // child's frame overlaps scratch the parent wrote after the
            // trap... precisely: fork copies the page, then the parent irets
            // and keeps running on ITS page (calls clobber below trapped
            // ESP); the child resumes on the SNAPSHOT with a live ESP that
            // points into clobbered... no — same snapshot...). Whatever the
            // mechanism: child-side stack writes before exec are UNRELIABLE
            // (the `p[270]` build faulted). So: exec DIRECTLY from the
            // inherited `line` buffer (written by the parent BEFORE the fork
            // trap — snapshot-clean), no child-side /bin/ prefix build.
            // (PATH fallback dropped: kernel resolves /bin/<name>... it
            // doesn't — shell_execute_run does, usys_exec doesn't. Type the
            // full path for now; sh-side prefix returns once the child
            // stack is proven.)
            usys_exec(line);
            puts1("exec failed\n");
            usys_exit(1);
        }
        // parent: wait for this child (-2 = parked, retry; -1 = no child)
        for (int i = 0; i < 100000000; i++) {
            int got = usys_wait(child);
            if (got == child) break;
            if (got == -1) break;
        }
    }
}
