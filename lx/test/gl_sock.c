#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <poll.h>

static int fails;
#define CHECK(c, what) do { int ok_ = !!(c); printf("%s %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) fails++; } while (0)

static void *server(void *arg) {
    int ls = (int)(long)arg;
    int c = accept(ls, 0, 0);
    char buf[64];
    int n = read(c, buf, sizeof buf);
    if (n > 0) { for (int i = 0; i < n; i++) buf[i] ^= 0x20; write(c, buf, n); }
    close(c);
    return 0;
}

int main(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    CHECK(write(sv[0], "ping", 4) == 4, "write to a socket");
    char b[16] = {0};
    CHECK(read(sv[1], b, 16) == 4 && !strcmp(b, "ping"), "read from its peer");

    // fork + socketpair
    pid_t p = fork();
    if (p == 0) { char x[8] = {0}; read(sv[1], x, 8); write(sv[1], x, strlen(x)); _exit(7); }
    write(sv[0], "abc", 3);
    char r[8] = {0};
    CHECK(read(sv[0], r, 8) == 3 && !strcmp(r, "abc"), "socket between parent and forked child");
    int st; waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7, "child exit status");

    // named socket server in a thread
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    strcpy(sa.sun_path, "/tmp/test.sock");
    CHECK(bind(ls, (struct sockaddr*)&sa, sizeof sa) == 0, "bind a named socket");
    CHECK(listen(ls, 4) == 0, "listen");
    pthread_t th;
    pthread_create(&th, 0, server, (void*)(long)ls);
    int cs = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(cs, (struct sockaddr*)&sa, sizeof sa) == 0, "connect");
    write(cs, "hello", 5);
    char rr[16] = {0};
    CHECK(read(cs, rr, 16) == 5 && !strcmp(rr, "HELLO"), "request/response through accept");
    pthread_join(th, 0);

    // fd passing (SCM_RIGHTS)
    int pp[2]; pipe(pp);
    struct msghdr m = {0};
    char dummy = 'x';
    struct iovec iov = { &dummy, 1 };
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof cbuf);
    m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = cbuf; m.msg_controllen = sizeof cbuf;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
    cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &pp[1], sizeof(int));
    CHECK(sendmsg(sv[0], &m, 0) == 1, "sendmsg with an fd");
    struct msghdr rm = {0};
    char rb; struct iovec riov = { &rb, 1 };
    char rcbuf[CMSG_SPACE(sizeof(int))];
    rm.msg_iov = &riov; rm.msg_iovlen = 1; rm.msg_control = rcbuf; rm.msg_controllen = sizeof rcbuf;
    CHECK(recvmsg(sv[1], &rm, 0) == 1, "recvmsg");
    struct cmsghdr *rc = CMSG_FIRSTHDR(&rm);
    int got = -1;
    if (rc && rc->cmsg_type == SCM_RIGHTS) memcpy(&got, CMSG_DATA(rc), sizeof(int));
    CHECK(got >= 0, "an fd arrived");
    write(got, "via-passed-fd", 13);
    char pb[32] = {0};
    CHECK(read(pp[0], pb, 32) == 13 && !strcmp(pb, "via-passed-fd"), "the passed fd is the same pipe");

    // epoll with an eventfd and a timerfd
    int ep = epoll_create1(0);
    int efd = eventfd(0, 0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = efd };
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) == 0, "epoll_ctl add");
    struct epoll_event out[4];
    CHECK(epoll_wait(ep, out, 4, 0) == 0, "epoll_wait: nothing ready");
    uint64_t one = 1; write(efd, &one, 8);
    CHECK(epoll_wait(ep, out, 4, 100) == 1 && out[0].data.fd == efd, "epoll_wait: eventfd ready");
    uint64_t v; read(efd, &v, 8);
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its = { .it_value = { 0, 50 * 1000 * 1000 } };
    timerfd_settime(tfd, 0, &its, 0);
    struct pollfd pf = { tfd, POLLIN, 0 };
    CHECK(poll(&pf, 1, 2000) == 1 && (pf.revents & POLLIN), "poll: timerfd fires after 50 ms");

    // select on a pipe
    fd_set rs; FD_ZERO(&rs); FD_SET(pp[0], &rs);
    struct timeval tv = { 0, 100000 };
    CHECK(select(pp[0] + 1, &rs, 0, 0, &tv) == 0, "select times out on an empty pipe");
    write(pp[1], "z", 1);
    FD_ZERO(&rs); FD_SET(pp[0], &rs);
    CHECK(select(pp[0] + 1, &rs, 0, 0, &tv) == 1, "select sees data");

    printf("%s\n", fails ? "SOCKTEST FAIL" : "SOCKTEST PASS");
    return fails != 0;
}
