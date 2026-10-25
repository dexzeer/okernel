// More Linux system calls: sockets, select/ppoll, epoll, eventfd, timerfd, memfd.
#include "lx.h"
#include "memory.h"
#include "serial.h"
#include "string.h"

extern int lx_fd_events(struct lx_task *t, struct lx_file *f, int want);
extern int lx_sock_new_file(struct lx_task *t, int domain, int type, int cloexec, int nonblock);
extern int64_t lx_sys_bind(struct lx_task *t, int fd, uint64_t uaddr, uint64_t alen);
extern int64_t lx_sys_listen(struct lx_task *t, int fd, int backlog);
extern int64_t lx_sys_connect(struct lx_task *t, int fd, uint64_t uaddr, uint64_t alen);
extern int64_t lx_sys_accept(struct lx_task *t, int fd, uint64_t uaddr, uint64_t ulen, int flags);
extern int64_t lx_sys_socketpair(struct lx_task *t, int domain, int type, int proto, uint64_t usv);
extern int64_t lx_sock_send(struct lx_task *t, int fd, uint64_t uaddr, uint64_t len, struct lx_file **fds, int nfds, uint64_t to_addr, uint64_t to_len);
extern int64_t lx_sock_recv(struct lx_task *t, int fd, uint64_t uaddr, uint64_t len, struct lx_file **rfds, int *nrfds, int peek);
extern int64_t lx_sock_shutdown(struct lx_task *t, int fd, int how);
extern int64_t lx_sock_name(struct lx_task *t, int fd, uint64_t uaddr, uint64_t ulen, int peer);
extern int64_t lx_sys_epoll_create(struct lx_task *t, int flags);
extern int64_t lx_sys_eventfd(struct lx_task *t, uint64_t initval, int flags);
extern int64_t lx_sys_timerfd_create(struct lx_task *t, int clock, int flags);
extern int64_t lx_sys_timerfd_settime(struct lx_task *t, int fd, int flags, uint64_t unew, uint64_t uold);
extern int64_t lx_sys_timerfd_gettime(struct lx_task *t, int fd, uint64_t uval);
extern int64_t lx_sys_inotify_init(struct lx_task *t, int flags);
extern int64_t lx_sys_epoll_ctl(struct lx_task *t, int epfd, int op, int fd, uint64_t uev);
extern int64_t lx_sys_epoll_wait(struct lx_task *t, int epfd, uint64_t uevents, int maxevents, int64_t timeout_ms);

static struct lx_file *fget(struct lx_task *t, int64_t fd) {
    return (fd >= 0 && fd < LX_MAX_FDS) ? t->files->fds[fd] : 0;
}

struct msghdr_k { uint64_t name; uint32_t namelen, pad; uint64_t iov; uint64_t iovlen; uint64_t control; uint64_t controllen; uint32_t flags, pad2; };

// ---- sendmsg / recvmsg with SCM_RIGHTS --------------------------------------------------------------------------------

static int64_t do_sendmsg(struct lx_task *t, int fd, uint64_t umsg, int flags) {
    (void)flags;
    struct msghdr_k m;
    if (lx_copy_in(t, &m, umsg, sizeof m)) return -LX_EFAULT;
    if (m.iovlen > 64) return -LX_EINVAL;
    struct lx_file *pass[8];
    int npass = 0;
    if (m.control && m.controllen >= 16) {
        uint8_t cbuf[256];
        uint64_t cl = m.controllen > sizeof cbuf ? sizeof cbuf : m.controllen;
        if (lx_copy_in(t, cbuf, m.control, cl)) return -LX_EFAULT;
        for (uint64_t off = 0; off + 16 <= cl; ) {
            uint64_t clen; int32_t level, type;
            memcpy(&clen, cbuf + off, 8); memcpy(&level, cbuf + off + 8, 4); memcpy(&type, cbuf + off + 12, 4);
            if (clen < 16 || off + clen > cl) break;
            if (level == 1 && type == 1) {                                // SOL_SOCKET, SCM_RIGHTS
                int n = (int)((clen - 16) / 4);
                for (int i = 0; i < n && npass < 8; i++) {
                    int32_t sfd; memcpy(&sfd, cbuf + off + 16 + i * 4, 4);
                    struct lx_file *pf = fget(t, sfd);
                    if (!pf) return -LX_EBADF;
                    pass[npass++] = pf;
                }
            }
            off += (clen + 7) & ~7ull;
        }
    }
    // gather the iovec into one buffer so a datagram stays one message
    uint64_t total = 0;
    struct { uint64_t base, len; } iov[64];
    if (m.iovlen && lx_copy_in(t, iov, m.iov, m.iovlen * 16)) return -LX_EFAULT;
    for (uint64_t i = 0; i < m.iovlen; i++) total += iov[i].len;
    if (total > 65536) total = 65536;
    uint8_t *tmp = total ? (uint8_t*)kmalloc((uint32_t)total) : 0;
    if (total && !tmp) return -LX_ENOMEM;
    uint64_t off = 0;
    for (uint64_t i = 0; i < m.iovlen && off < total; i++) {
        uint64_t k = iov[i].len > total - off ? total - off : iov[i].len;
        if (lx_copy_in(t, tmp + off, iov[i].base, k)) { kfree(tmp); return -LX_EFAULT; }
        off += k;
    }
    // sock_send reads user memory; give it a stand-in by writing the gathered data to a scratch user-visible path: use the
    // task's own stack-free approach -- copy through a kernel-side wrapper instead
    extern int64_t lx_sock_send_kernel(struct lx_task *t, int fd, const uint8_t *buf, uint64_t len, struct lx_file **fds, int nfds, uint64_t to_addr, uint64_t to_len);
    int64_t r = lx_sock_send_kernel(t, fd, tmp, total, pass, npass, m.name, m.namelen);
    if (tmp) kfree(tmp);
    return r;
}

static int64_t do_recvmsg(struct lx_task *t, int fd, uint64_t umsg, int flags) {
    struct msghdr_k m;
    if (lx_copy_in(t, &m, umsg, sizeof m)) return -LX_EFAULT;
    if (m.iovlen > 64) return -LX_EINVAL;
    struct { uint64_t base, len; } iov[64];
    if (m.iovlen && lx_copy_in(t, iov, m.iov, m.iovlen * 16)) return -LX_EFAULT;
    struct lx_file *got[8];
    int ngot = 0;
    extern int64_t lx_sock_recv_iov(struct lx_task *t, int fd, uint64_t *iov, uint64_t iovlen, struct lx_file **rfds, int *nrfds, int peek);
    int64_t r = lx_sock_recv_iov(t, fd, (uint64_t*)iov, m.iovlen, got, &ngot, (flags & 2) != 0);
    if (r < 0 || t->state == LXT_BLOCK) return r;
    m.flags = 0;
    if (m.control && m.controllen >= 24 && ngot > 0) {
        uint8_t cbuf[256];
        int fit = (int)((m.controllen - 16) / 4);
        if (fit > ngot) fit = ngot;
        if (fit > 8) fit = 8;
        memset(cbuf, 0, sizeof cbuf);
        uint64_t clen = 16 + 4 * (uint64_t)fit;
        int32_t level = 1, type = 1;
        memcpy(cbuf, &clen, 8); memcpy(cbuf + 8, &level, 4); memcpy(cbuf + 12, &type, 4);
        for (int i = 0; i < fit; i++) {
            got[i]->refs++;
            int nfd = lx_fd_alloc(t, got[i], 0, (flags & 0x40000000) != 0);           // MSG_CMSG_CLOEXEC
            if (nfd < 0) { lx_file_put(got[i]); nfd = -1; }
            memcpy(cbuf + 16 + i * 4, &nfd, 4);
        }
        lx_copy_out(t, m.control, cbuf, (clen + 7) & ~7ull);
        m.controllen = (clen + 7) & ~7ull;
    } else m.controllen = 0;
    for (int i = 0; i < ngot; i++) lx_file_put(got[i]);                                // the queue's reference
    m.namelen = 0;
    lx_copy_out(t, umsg, &m, sizeof m);
    return r;
}

// ---- select / ppoll -----------------------------------------------------------------------------------------------------

static int select_ready(struct lx_task *t) {
    if (t->wake_ms && lx_now_ms() >= (int64_t)t->wake_ms) return 1;
    int n = (int)t->regs.rdi;
    uint64_t sets[3] = { t->regs.rsi, t->regs.rdx, t->regs.r10 };
    int want[3] = { 1, 4, 0 };
    for (int s = 0; s < 3; s++) {
        if (!sets[s]) continue;
        uint8_t bits[128];
        int nb = (n + 7) / 8; if (nb > 128) nb = 128;
        if (lx_copy_in(t, bits, sets[s], (uint64_t)nb)) return 1;
        for (int fd = 0; fd < n && fd < 1024; fd++) {
            if (!(bits[fd / 8] & (1 << (fd % 8)))) continue;
            struct lx_file *f = fget(t, fd);
            if (!f) return 1;
            if (s < 2 && (lx_fd_events(t, f, want[s]) & want[s])) return 1;
        }
    }
    return 0;
}

struct tv_k { int64_t sec, sub; };

static int64_t do_select(struct lx_task *t, int n, uint64_t ur, uint64_t uw, uint64_t ue, uint64_t utv, int is_ts) {
    if (n < 0 || n > 1024) return -LX_EINVAL;
    int nb = (n + 7) / 8;
    uint8_t in[3][128], out[3][128];
    uint64_t up[3] = { ur, uw, ue };
    int want[3] = { 1, 4, 0 };
    int ready = 0;
    memset(out, 0, sizeof out);
    for (int s = 0; s < 3; s++) {
        memset(in[s], 0, sizeof in[s]);
        if (up[s] && lx_copy_in(t, in[s], up[s], (uint64_t)nb)) return -LX_EFAULT;
    }
    for (int s = 0; s < 3; s++)
        for (int fd = 0; fd < n; fd++) {
            if (!(in[s][fd / 8] & (1 << (fd % 8)))) continue;
            struct lx_file *f = fget(t, fd);
            if (!f) return -LX_EBADF;
            if (s < 2 && (lx_fd_events(t, f, want[s]) & want[s])) { out[s][fd / 8] |= (uint8_t)(1 << (fd % 8)); ready++; }
        }
    int64_t timeout_ms = -1;
    if (utv) {
        struct tv_k tv;
        if (lx_copy_in(t, &tv, utv, 16)) return -LX_EFAULT;
        timeout_ms = tv.sec * 1000 + (is_ts ? tv.sub / 1000000 : tv.sub / 1000);
    }
    if (ready || timeout_ms == 0) {
        t->wake_ms = 0;
        for (int s = 0; s < 3; s++) if (up[s] && lx_copy_out(t, up[s], out[s], (uint64_t)nb)) return -LX_EFAULT;
        return ready;
    }
    if (timeout_ms > 0) {
        if (!t->wake_ms) t->wake_ms = (uint64_t)(lx_now_ms() + timeout_ms);
        else if (lx_now_ms() >= (int64_t)t->wake_ms) {
            t->wake_ms = 0;
            for (int s = 0; s < 3; s++) if (up[s] && lx_copy_out(t, up[s], out[s], (uint64_t)nb)) return -LX_EFAULT;
            return 0;
        }
    }
    lx_block(t, select_ready, 0);
    return 0;
}

struct pollfd_k { int32_t fd; int16_t events, revents; };

static int ppoll_ready(struct lx_task *t) {
    if (t->wake_ms && lx_now_ms() >= (int64_t)t->wake_ms) return 1;
    uint64_t u = t->regs.rdi; uint32_t n = (uint32_t)t->regs.rsi;
    for (uint32_t i = 0; i < n && i < 256; i++) {
        struct pollfd_k p;
        if (lx_copy_in(t, &p, u + i * 8, 8)) return 1;
        if (p.fd >= 0 && lx_fd_events(t, fget(t, p.fd), p.events)) return 1;
    }
    return 0;
}

static int64_t do_ppoll(struct lx_task *t, uint64_t ufds, uint64_t n, uint64_t uts) {
    if (n > 256) return -LX_EINVAL;
    int ready = 0;
    for (uint64_t i = 0; i < n; i++) {
        struct pollfd_k p;
        if (lx_copy_in(t, &p, ufds + i * 8, 8)) return -LX_EFAULT;
        p.revents = 0;
        if (p.fd >= 0) p.revents = (int16_t)lx_fd_events(t, fget(t, p.fd), p.events);
        if (p.revents) ready++;
        if (lx_copy_out(t, ufds + i * 8 + 6, &p.revents, 2)) return -LX_EFAULT;
    }
    int64_t timeout_ms = -1;
    if (uts) { struct tv_k ts; if (lx_copy_in(t, &ts, uts, 16)) return -LX_EFAULT; timeout_ms = ts.sec * 1000 + ts.sub / 1000000; }
    if (ready || timeout_ms == 0) { t->wake_ms = 0; return ready; }
    if (timeout_ms > 0) {
        if (!t->wake_ms) t->wake_ms = (uint64_t)(lx_now_ms() + timeout_ms);
        else if (lx_now_ms() >= (int64_t)t->wake_ms) { t->wake_ms = 0; return 0; }
    }
    lx_block(t, ppoll_ready, 0);
    return 0;
}

// ---- memfd -------------------------------------------------------------------------------------------------------------

static int64_t do_memfd(struct lx_task *t, uint64_t uname, uint32_t flags) {
    char name[64];
    int n = lx_copy_str(t, name, uname, sizeof name);
    if (n < 0) return n;
    struct lx_node *tmp = lxfs_root(), *dir = 0; int err = 0;
    dir = lxfs_resolve(tmp, "/tmp", 1, &err);
    if (!dir) dir = tmp;
    static int memfd_n;
    char fname[96] = ".memfd-";
    int l = 7;
    int v = ++memfd_n;
    char dg[12]; int dn = 0;
    while (v) { dg[dn++] = (char)('0' + v % 10); v /= 10; }
    while (dn) fname[l++] = dg[--dn];
    fname[l] = 0;
    struct lx_node *node = lxfs_create(dir, fname, LXN_REG, 0600, &err);
    if (!node) return err;
    struct lx_file *f = lx_file_new(LXF_NODE, LX_O_RDWR, node);
    if (!f) return -LX_ENOMEM;
    lxfs_unlink(dir, fname, 0);                                       // anonymous: lives while the file is open
    int fd = lx_fd_alloc(t, f, 0, (flags & 1) != 0);
    if (fd < 0) lx_file_put(f);
    return fd;
}

// ---- sockets and the rest ----------------------------------------------------------------------------------------------

int lx_fd_events_pub(struct lx_task *t, struct lx_file *f, int want) { return lx_fd_events(t, f, want); }

int64_t lx_syscall_more(struct lx_task *t, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    switch (nr) {
    case 41: { // socket
        int dom = (int)a0, type = (int)a1;
        if (dom != 1 && dom != 2) return -97;                          // EAFNOSUPPORT
        int base = type & 0xFF;
        if (base != 1 && base != 2 && base != 5) return -94;
        return lx_sock_new_file(t, dom, base, (type & 02000000) != 0, (type & 04000) != 0);
    }
    case 42: return lx_sys_connect(t, (int)a0, a1, a2);
    case 43: return lx_sys_accept(t, (int)a0, a1, a2, 0);
    case 288: return lx_sys_accept(t, (int)a0, a1, a2, (int)a3);
    case 44: return lx_sock_send(t, (int)a0, a1, a2, 0, 0, a4, a5);                // sendto
    case 45: { // recvfrom
        int64_t r = lx_sock_recv(t, (int)a0, a1, a2, 0, 0, (a3 & 2) != 0);
        if (r >= 0 && a4 && a5) { uint32_t zero = 0; lx_copy_out(t, a5, &zero, 4); }
        return r;
    }
    case 46: return do_sendmsg(t, (int)a0, a1, (int)a2);
    case 47: return do_recvmsg(t, (int)a0, a1, (int)a2);
    case 48: return lx_sock_shutdown(t, (int)a0, (int)a1);
    case 49: return lx_sys_bind(t, (int)a0, a1, a2);
    case 50: return lx_sys_listen(t, (int)a0, (int)a1);
    case 51: return lx_sock_name(t, (int)a0, a1, a2, 0);
    case 52: return lx_sock_name(t, (int)a0, a1, a2, 1);
    case 53: return lx_sys_socketpair(t, (int)a0, (int)a1, (int)a2, a3);
    case 54: return 0;                                                              // setsockopt
    case 55: { // getsockopt(fd, level, name, val, len)
        struct lx_file *f = fget(t, (int64_t)a0);
        if (!f || f->kind != LXF_SOCK) return -88;
        int32_t v = 0;
        if (a1 == 1 && a2 == 3) v = 1;                                              // SO_TYPE: stream (callers mostly test SO_ERROR)
        if (a3 && lx_copy_out(t, a3, &v, 4)) return -LX_EFAULT;
        if (a4) { uint32_t l = 4; lx_copy_out(t, a4, &l, 4); }
        return 0;
    }
    case 23: return do_select(t, (int)a0, a1, a2, a3, a4, 0);
    case 270: return do_select(t, (int)a0, a1, a2, a3, a4, 1);                      // pselect6
    case 271: return do_ppoll(t, a0, a1, a2);
    case 213: case 291: return lx_sys_epoll_create(t, nr == 291 ? (int)a0 : 0);
    case 233: return lx_sys_epoll_ctl(t, (int)a0, (int)a1, (int)a2, a3);
    case 232: case 281: return lx_sys_epoll_wait(t, (int)a0, a1, (int)a2, (int64_t)(int32_t)a3);
    case 284: return lx_sys_eventfd(t, a0, 0);
    case 290: return lx_sys_eventfd(t, a0, (int)a1);
    case 283: return lx_sys_timerfd_create(t, (int)a0, (int)a1);
    case 286: return lx_sys_timerfd_settime(t, (int)a0, (int)a1, a2, a3);
    case 287: return lx_sys_timerfd_gettime(t, (int)a0, a1);
    case 253: return lx_sys_inotify_init(t, 0);
    case 294: return lx_sys_inotify_init(t, (int)a0);
    case 254: return 1;                                                             // inotify_add_watch
    case 255: return 0;
    case 319: return do_memfd(t, a0, (uint32_t)a1);
    case 221: case 285: return 0;                                                   // fadvise64, fallocate
    case 160: case 161: return 0;                                                   // setrlimit, chroot
    case 169: case 170: case 171: return 0;                                         // hostname/domain setters
    case 203: case 142: case 144: case 145: case 146: case 147: case 148: return 0; // sched_*: accepted
    case 140: case 141: return 0;                                                   // getpriority, setpriority
    case 157: return 0;                                                             // prctl: PR_SET_NAME and friends
    case 100: { uint64_t tms[4] = { 0, 0, 0, 0 }; if (a0 && lx_copy_out(t, a0, tms, 32)) return -LX_EFAULT; return (int64_t)(lx_now_ms() / 10); }   // times
    case 139: return -LX_ENOSYS;                                                    // sysfs
    default:
        serial_printf("[lx] pid %d (%s): unimplemented syscall %d\n", t->tgid, t->comm, (int)nr);
        return -LX_ENOSYS;
    }
}
