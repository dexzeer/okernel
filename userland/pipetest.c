// pipetest.c — prove pipe()/read()/write() (/bin/pipetest).
// Creates a pipe, writes a message on the write end, reads it back on the
// read end, prints what came back. Single-process loopback (no fork needed).

static int sys_print(const char *msg) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(0), "b"(msg) : "memory", "ecx", "edx");
    return r;
}

static int sys_write(int fd, const char *buf, unsigned len) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r) : "a"(2), "b"(fd), "c"(buf), "d"(len)
                     : "memory");
    return r;
}

static int sys_pipe(int *fds) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(12), "b"(fds)
                     : "memory", "ecx", "edx");
    return r;
}

static int sys_read(int fd, char *buf, unsigned len) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(6), "b"(fd), "c"(buf), "d"(len)
                     : "memory");
    return r;
}

static void sys_exit(int code) {
    __asm__ volatile("int $0x80" : : "a"(1), "b"(code) : "memory");
    while (1) { }
}

void _start(void) {
    int fds[2] = { -1, -1 };
    if (sys_pipe(fds) != 0) {
        sys_print("pipetest: pipe failed");
        sys_exit(1);
    }
    const char *msg = "hello pipe";
    unsigned len = 10;
    int w = sys_write(fds[1], msg, len);
    if (w != 10) {
        sys_print("pipetest: short write");
        sys_exit(1);
    }
    char buf[32];
    for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = 0;
    int n = sys_read(fds[0], buf, sizeof(buf) - 1);
    if (n != 10) {
        sys_print("pipetest: short read");
        sys_exit(1);
    }
    // verify content matches
    for (int i = 0; i < 10; i++) {
        if (buf[i] != msg[i]) {
            sys_print("pipetest: MISMATCH");
            sys_exit(1);
        }
    }
    sys_print("pipetest: loopback OK");
    sys_write(1, buf, n);
    sys_exit(0);
}
