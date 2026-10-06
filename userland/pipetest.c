// pipetest.c — prove pipe()/read()/write() (/bin/pipetest).
// Creates a pipe, writes a message on the write end, reads it back on the
// read end, prints what came back. Single-process loopback (no fork needed).

#include "usys.h"

void _start(void) {
    int fds[2] = { -1, -1 };
    if (usys_pipe(fds) != 0) {
        usys_print("pipetest: pipe failed");
        usys_exit(1);
    }
    const char *msg = "hello pipe";
    unsigned len = 10;
    int w = usys_write(fds[1], msg, len);
    if (w != 10) {
        usys_print("pipetest: short write");
        usys_exit(1);
    }
    char buf[32];
    for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = 0;
    int n = usys_read(fds[0], buf, sizeof(buf) - 1);
    if (n != 10) {
        usys_print("pipetest: short read");
        usys_exit(1);
    }
    // verify content matches
    for (int i = 0; i < 10; i++) {
        if (buf[i] != msg[i]) {
            usys_print("pipetest: MISMATCH");
            usys_exit(1);
        }
    }
    usys_print("pipetest: loopback OK");
    usys_write(1, buf, n);
    usys_exit(0);
}
