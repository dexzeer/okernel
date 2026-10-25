// Sockets of the Linux subsystem. AF_UNIX (stream, datagram, socketpair, bound names, fd passing) is complete in memory;
// AF_INET talks to the kernel's TCP/UDP stack.
#include "lx.h"
#include "memory.h"
#include "serial.h"
#include "string.h"
#include "net/network.h"

#define AF_UNIX 1
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC 02000000

#define SOCK_BUF 65536
#define MAX_PASSED 8

struct lx_msg_fds { int n; struct lx_file *f[MAX_PASSED]; };

// A byte ring with the fds that travel with it (SCM_RIGHTS): an fd list is attached to the position of its first byte.
struct ring {
    uint8_t *buf;
    uint32_t rd, wr, count;
    struct { uint32_t pos; struct lx_msg_fds fds; } pend[4];     // fds not yet received, by absolute byte position
    uint32_t npend;
    uint64_t total_in, total_out;
};

struct lx_sock {
    int domain, type;
    int refs;
    int state;                       // 0 new, 1 bound, 2 listening, 3 connected, 4 closed
    struct ring in;                  // what this end receives
    struct lx_sock *peer;
    // listening
    struct lx_sock *backlog[16];
    int nback;
    char path[108];
    int abstract;
    struct lx_node *node;
    int shut_rd, shut_wr;
    // datagram messages queue (lengths in `in` as 4-byte prefixes)
    int pass_cred;
    // inet
    int tcp;                         // kernel TCP socket id, -1 none
    uint32_t rip; uint16_t rport;    // remote address (host order)
    uint16_t lport;                  // local port
    int udp_bound;
};

static struct lx_sock *abstract_tab[16];
static int parse_in(struct lx_task *t, uint64_t uaddr, uint64_t alen, uint32_t *ip, uint16_t *port);
static uint16_t swap16(uint16_t v);

static int ring_init(struct ring *r) {
    r->buf = (uint8_t*)kmalloc(SOCK_BUF);
    if (!r->buf) return -1;
    r->rd = r->wr = r->count = 0;
    r->npend = 0; r->total_in = r->total_out = 0;
    return 0;
}

static struct lx_sock *sock_new(int domain, int type) {
    struct lx_sock *s = (struct lx_sock*)kcalloc(1, sizeof *s);
    if (!s) return 0;
    s->domain = domain; s->type = type; s->refs = 1; s->tcp = -1;
    if (ring_init(&s->in)) { kfree(s); return 0; }
    return s;
}

static void sock_free(struct lx_sock *s) {
    for (uint32_t i = 0; i < s->in.npend; i++) for (int k = 0; k < s->in.pend[i].fds.n; k++) lx_file_put(s->in.pend[i].fds.f[k]);
    if (s->peer && s->peer->peer == s) s->peer->peer = 0;
    for (int i = 0; i < 16; i++) if (abstract_tab[i] == s) abstract_tab[i] = 0;
    if (s->node && s->node->data == (uint8_t*)s) s->node->data = 0;
    if (s->tcp >= 0) tcp_close(s->tcp);
    if (s->udp_bound) udp_unbind_port(s->lport);
    kfree(s->in.buf);
    kfree(s);
}

void lx_sock_put(struct lx_sock *s) { if (--s->refs <= 0) sock_free(s); }

int lx_sock_new_file(struct lx_task *t, int domain, int type, int cloexec, int nonblock) {
    struct lx_sock *s = sock_new(domain, type);
    if (!s) return -LX_ENOMEM;
    struct lx_file *f = lx_file_new(LXF_SOCK, LX_O_RDWR | (nonblock ? LX_O_NONBLOCK : 0), 0);
    if (!f) { lx_sock_put(s); return -LX_ENOMEM; }
    f->sock = s;
    int fd = lx_fd_alloc(t, f, 0, cloexec);
    if (fd < 0) lx_file_put(f);
    return fd;
}

// ---- ring I/O -----------------------------------------------------------------------------------------------------

static uint32_t ring_write(struct ring *r, const uint8_t *p, uint32_t n) {
    uint32_t room = SOCK_BUF - r->count;
    if (n > room) n = room;
    for (uint32_t i = 0; i < n; i++) { r->buf[r->wr] = p[i]; r->wr = (r->wr + 1) % SOCK_BUF; }
    r->count += n; r->total_in += n;
    return n;
}

static uint32_t ring_read(struct ring *r, uint8_t *p, uint32_t n) {
    if (n > r->count) n = r->count;
    for (uint32_t i = 0; i < n; i++) { p[i] = r->buf[r->rd]; r->rd = (r->rd + 1) % SOCK_BUF; }
    r->count -= n; r->total_out += n;
    return n;
}

// ---- readiness ------------------------------------------------------------------------------------------------------

int lx_sock_readable(struct lx_sock *s) {
    if (s->state == 2) return s->nback > 0;
    if (s->domain == AF_INET && s->type == SOCK_STREAM) {
        extern int lx_inet_readable(struct lx_sock *s);
        return lx_inet_readable(s);
    }
    return s->in.count > 0 || s->shut_rd || (s->state == 3 && !s->peer) || s->state == 4;
}

int lx_sock_writable(struct lx_sock *s) {
    if (s->state == 2) return 0;
    if (s->peer) return s->peer->in.count < SOCK_BUF;
    return s->state == 3 || s->domain == AF_INET;
}

// ---- wait predicates ------------------------------------------------------------------------------------------------

static struct lx_sock *sock_of(struct lx_task *t, int64_t fd) {
    if (fd < 0 || fd >= LX_MAX_FDS) return 0;
    struct lx_file *f = t->files->fds[fd];
    return f && f->kind == LXF_SOCK ? f->sock : 0;
}

static int wait_readable(struct lx_task *t) {
    struct lx_sock *s = sock_of(t, (int64_t)t->block_arg);
    return !s || lx_sock_readable(s);
}

static int wait_writable(struct lx_task *t) {
    struct lx_sock *s = sock_of(t, (int64_t)t->block_arg);
    return !s || lx_sock_writable(s) || !s->peer;
}

// ---- unix sockets: bind, listen, connect, accept -------------------------------------------------------------------------

static int unix_path(struct lx_task *t, uint64_t uaddr, uint64_t alen, char *out, int *abstract) {
    if (alen < 2 || alen > 110) return -LX_EINVAL;
    uint8_t sa[112];
    if (lx_copy_in(t, sa, uaddr, alen)) return -LX_EFAULT;
    uint16_t fam; memcpy(&fam, sa, 2);
    if (fam != AF_UNIX) return -97;                                    // EAFNOSUPPORT
    uint32_t n = (uint32_t)alen - 2;
    if (n && sa[2] == 0) {                                             // abstract namespace
        *abstract = 1;
        if (n > 107) n = 107;
        memcpy(out, sa + 3, n - 1); out[n - 1] = 0;
        return 0;
    }
    *abstract = 0;
    uint32_t l = 0;
    while (l < n && sa[2 + l]) { out[l] = (char)sa[2 + l]; l++; }
    out[l] = 0;
    return 0;
}

static int abstract_find(const char *name) {
    for (int i = 0; i < 16; i++) if (abstract_tab[i] && strcmp(abstract_tab[i]->path, name) == 0) return i;
    return -1;
}

static int udp_ensure_bound(struct lx_sock *s);
static int udp_ensure_bound_pub(struct lx_sock *s) { return udp_ensure_bound(s); }

int64_t lx_sys_bind(struct lx_task *t, int fd, uint64_t uaddr, uint64_t alen) {
    struct lx_sock *s = sock_of(t, fd);
    if (!s) return -LX_EBADF;
    if (s->domain == AF_INET) {
        uint32_t ip; uint16_t port;
        int pr = parse_in(t, uaddr, alen, &ip, &port);
        if (pr) return pr;
        s->lport = port;
        s->state = 1;
        if (s->type == SOCK_DGRAM && port) return udp_ensure_bound_pub(s);
        return 0;
    }
    char path[108]; int abs;
    int r = unix_path(t, uaddr, alen, path, &abs);
    if (r) return r;
    if (s->state != 0) return -LX_EINVAL;
    if (abs) {
        if (abstract_find(path) >= 0) return -98;                      // EADDRINUSE
        int slot = -1;
        for (int i = 0; i < 16; i++) if (!abstract_tab[i]) { slot = i; break; }
        if (slot < 0) return -LX_ENOMEM;
        strncpy(s->path, path, 107);
        s->abstract = 1;
        abstract_tab[slot] = s;
    } else {
        struct lx_node *dir; char name[LX_NAME_MAX + 1];
        r = lxfs_resolve_parent(t->cwd, path, &dir, name);
        if (r) return r;
        int err = 0;
        struct lx_node *n = lxfs_create(dir, name, LXN_SOCK, 0777, &err);
        if (!n) return err == -LX_EEXIST ? -98 : err;
        n->data = (uint8_t*)s;                                         // the node points at the socket that owns the name
        s->node = n;
        strncpy(s->path, path, 107);
    }
    s->state = 1;
    return 0;
}

int64_t lx_sys_listen(struct lx_task *t, int fd, int backlog) {
    struct lx_sock *s = sock_of(t, fd);
    if (!s) return -LX_EBADF;
    (void)backlog;
    if (s->domain == AF_UNIX && s->state != 1) return -LX_EINVAL;
    s->state = 2;
    return 0;
}

int64_t lx_sys_connect(struct lx_task *t, int fd, uint64_t uaddr, uint64_t alen) {
    struct lx_sock *s = sock_of(t, fd);
    if (!s) return -LX_EBADF;
    if (s->domain == AF_INET) {
        extern int64_t lx_inet_connect(struct lx_task *t, struct lx_sock *s, uint64_t uaddr, uint64_t alen);
        return lx_inet_connect(t, s, uaddr, alen);
    }
    char path[108]; int abs;
    int r = unix_path(t, uaddr, alen, path, &abs);
    if (r) return r;
    struct lx_sock *l = 0;
    if (abs) { int i = abstract_find(path); if (i >= 0) l = abstract_tab[i]; }
    else {
        int err = 0;
        struct lx_node *n = lxfs_resolve(t->cwd, path, 1, &err);
        if (!n) return err;
        if (n->type != LXN_SOCK) return -111;                          // ECONNREFUSED
        l = (struct lx_sock*)n->data;
    }
    if (!l) return -111;
    if (s->type == SOCK_DGRAM) {                                       // datagram: "connected" just remembers the peer
        s->peer = l; s->state = 3;
        return 0;
    }
    if (l->state != 2) return -111;
    if (l->nback >= 16) return -LX_EAGAIN;
    struct lx_sock *srv = sock_new(AF_UNIX, s->type);
    if (!srv) return -LX_ENOMEM;
    srv->state = 3;
    s->peer = srv; srv->peer = s; s->state = 3;
    l->backlog[l->nback++] = srv;
    return 0;
}

static int accept_ready(struct lx_task *t) {
    struct lx_sock *s = sock_of(t, (int64_t)t->block_arg);
    return !s || s->nback > 0;
}

int64_t lx_sys_accept(struct lx_task *t, int fd, uint64_t uaddr, uint64_t ulen, int flags) {
    struct lx_file *lf = (fd >= 0 && fd < LX_MAX_FDS) ? t->files->fds[fd] : 0;
    struct lx_sock *s = lf && lf->kind == LXF_SOCK ? lf->sock : 0;
    if (!s) return -LX_EBADF;
    if (s->state != 2) return -LX_EINVAL;
    if (s->domain == AF_INET) {
        extern int64_t lx_inet_accept(struct lx_task *t, struct lx_sock *s, int flags);
        return lx_inet_accept(t, s, flags);
    }
    if (s->nback == 0) {
        if (lf->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
        lx_block(t, accept_ready, (uint64_t)fd);
        return 0;
    }
    struct lx_sock *c = s->backlog[0];
    for (int i = 1; i < s->nback; i++) s->backlog[i - 1] = s->backlog[i];
    s->nback--;
    struct lx_file *f = lx_file_new(LXF_SOCK, LX_O_RDWR | ((flags & SOCK_NONBLOCK) ? LX_O_NONBLOCK : 0), 0);
    if (!f) { lx_sock_put(c); return -LX_ENOMEM; }
    f->sock = c;
    int nfd = lx_fd_alloc(t, f, 0, (flags & SOCK_CLOEXEC) != 0);
    if (nfd < 0) { lx_file_put(f); return nfd; }
    if (uaddr && ulen) { uint32_t zero = 0; lx_copy_out(t, ulen, &zero, 4); }
    return nfd;
}

int64_t lx_sys_socketpair(struct lx_task *t, int domain, int type, int proto, uint64_t usv) {
    (void)proto;
    if (domain != AF_UNIX) return -97;
    int base = type & 0xFF;
    if (base != SOCK_STREAM && base != SOCK_DGRAM && base != SOCK_SEQPACKET) return -94;     // ESOCKTNOSUPPORT
    struct lx_sock *a = sock_new(AF_UNIX, base), *b = sock_new(AF_UNIX, base);
    struct lx_file *fa = a ? lx_file_new(LXF_SOCK, LX_O_RDWR | ((type & SOCK_NONBLOCK) ? LX_O_NONBLOCK : 0), 0) : 0;
    struct lx_file *fb = b ? lx_file_new(LXF_SOCK, LX_O_RDWR | ((type & SOCK_NONBLOCK) ? LX_O_NONBLOCK : 0), 0) : 0;
    if (!a || !b || !fa || !fb) return -LX_ENOMEM;
    a->peer = b; b->peer = a; a->state = b->state = 3;
    fa->sock = a; fb->sock = b;
    int x = lx_fd_alloc(t, fa, 0, (type & SOCK_CLOEXEC) != 0);
    int y = x >= 0 ? lx_fd_alloc(t, fb, 0, (type & SOCK_CLOEXEC) != 0) : -LX_EMFILE;
    if (x < 0 || y < 0) { if (x >= 0) { t->files->fds[x] = 0; } lx_file_put(fa); lx_file_put(fb); return -LX_EMFILE; }
    int32_t sv[2] = { x, y };
    return lx_copy_out(t, usv, sv, 8);
}

// ---- data -----------------------------------------------------------------------------------------------------------

// Send `len` bytes at `uaddr`, with an optional list of fds (SCM_RIGHTS). Returns bytes sent, or blocks.
static const uint8_t *g_kbuf;                                          // set while sending from a kernel buffer
static int src_copy(struct lx_task *t, uint8_t *dst, uint64_t uaddr, uint32_t k) {
    if (g_kbuf) { memcpy(dst, g_kbuf + uaddr, k); return 0; }
    return lx_copy_in(t, dst, uaddr, k);
}

int64_t lx_sock_send(struct lx_task *t, int fd, uint64_t uaddr, uint64_t len, struct lx_file **fds, int nfds, uint64_t to_addr, uint64_t to_len) {
    struct lx_file *f = (fd >= 0 && fd < LX_MAX_FDS) ? t->files->fds[fd] : 0;
    struct lx_sock *s = f && f->kind == LXF_SOCK ? f->sock : 0;
    if (!s) return -LX_EBADF;
    if (s->domain == AF_INET) {
        extern int64_t lx_inet_send(struct lx_task *t, struct lx_file *f, struct lx_sock *s, uint64_t uaddr, uint64_t len, uint64_t to_addr, uint64_t to_len);
        return lx_inet_send(t, f, s, uaddr, len, to_addr, to_len);
    }
    (void)to_addr; (void)to_len;
    struct lx_sock *p = s->peer;
    if (!p || s->shut_wr) { lx_signal(t, 13); return -LX_EPIPE; }
    uint8_t buf[4096];
    int64_t done = 0;
    if (s->type == SOCK_DGRAM || s->type == SOCK_SEQPACKET) {            // one message: length prefix + bytes, all or nothing
        if (len + 4 > SOCK_BUF) return -90;                              // EMSGSIZE
        if (p->in.count + len + 4 > SOCK_BUF) {
            if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
            lx_block(t, wait_writable, (uint64_t)fd);
            return 0;
        }
        uint32_t l32 = (uint32_t)len;
        ring_write(&p->in, (uint8_t*)&l32, 4);
        for (uint64_t off = 0; off < len; ) {
            uint32_t k = len - off > sizeof buf ? sizeof buf : (uint32_t)(len - off);
            if (src_copy(t, buf, uaddr + off, k)) return -LX_EFAULT;
            ring_write(&p->in, buf, k);
            off += k;
        }
        return (int64_t)len;
    }
    if (p->in.count == SOCK_BUF && len) {
        if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
        lx_block(t, wait_writable, (uint64_t)fd);
        return 0;
    }
    if (nfds > 0 && p->in.npend < 4) {                                   // fds ride with the first byte of this chunk
        int i = (int)p->in.npend++;
        p->in.pend[i].pos = (uint32_t)p->in.total_in;
        p->in.pend[i].fds.n = nfds;
        for (int k = 0; k < nfds; k++) { p->in.pend[i].fds.f[k] = fds[k]; fds[k]->refs++; }
    }
    while (len && p->in.count < SOCK_BUF) {
        uint32_t k = len > sizeof buf ? sizeof buf : (uint32_t)len;
        if (k > SOCK_BUF - p->in.count) k = SOCK_BUF - p->in.count;
        if (src_copy(t, buf, uaddr, k)) return done ? done : -LX_EFAULT;
        k = ring_write(&p->in, buf, k);
        uaddr += k; len -= k; done += k;
    }
    return done;
}

// Receive into `uaddr`. fds the sender attached come back in rfds (up to MAX_PASSED). Returns bytes, 0 at EOF; blocks when empty.
int64_t lx_sock_recv(struct lx_task *t, int fd, uint64_t uaddr, uint64_t len, struct lx_file **rfds, int *nrfds, int peek) {
    struct lx_file *f = (fd >= 0 && fd < LX_MAX_FDS) ? t->files->fds[fd] : 0;
    struct lx_sock *s = f && f->kind == LXF_SOCK ? f->sock : 0;
    if (!s) return -LX_EBADF;
    if (nrfds) *nrfds = 0;
    if (s->domain == AF_INET) {
        extern int64_t lx_inet_recv(struct lx_task *t, struct lx_file *f, struct lx_sock *s, uint64_t uaddr, uint64_t len);
        return lx_inet_recv(t, f, s, uaddr, len);
    }
    if (s->in.count == 0) {
        if (s->shut_rd || !s->peer || s->state == 4) return 0;           // end of file
        if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
        lx_block(t, wait_readable, (uint64_t)fd);
        return 0;
    }
    uint8_t buf[4096];
    int64_t done = 0;
    if (s->type == SOCK_DGRAM || s->type == SOCK_SEQPACKET) {
        uint32_t mlen;
        uint32_t rd = s->in.rd, cnt = s->in.count;
        if (cnt < 4) return 0;
        uint8_t hdr[4];
        for (int i = 0; i < 4; i++) { hdr[i] = s->in.buf[rd]; rd = (rd + 1) % SOCK_BUF; }
        memcpy(&mlen, hdr, 4);
        if (peek) {
            uint32_t n = mlen < len ? mlen : (uint32_t)len;
            for (uint32_t off = 0; off < n; ) { uint32_t k = n - off > sizeof buf ? sizeof buf : n - off; for (uint32_t i = 0; i < k; i++) { buf[i] = s->in.buf[rd]; rd = (rd + 1) % SOCK_BUF; } if (lx_copy_out(t, uaddr + off, buf, k)) return -LX_EFAULT; off += k; }
            return n;
        }
        ring_read(&s->in, hdr, 4);
        uint32_t n = mlen < len ? mlen : (uint32_t)len;
        for (uint32_t off = 0; off < n; ) {
            uint32_t k = n - off > sizeof buf ? sizeof buf : n - off;
            ring_read(&s->in, buf, k);
            if (lx_copy_out(t, uaddr + off, buf, k)) return -LX_EFAULT;
            off += k;
        }
        for (uint32_t off = n; off < mlen; ) { uint32_t k = mlen - off > sizeof buf ? sizeof buf : mlen - off; ring_read(&s->in, buf, k); off += k; }   // truncated tail is dropped
        return n;
    }
    // stream: fds attached at positions up to the end of what we read arrive with this read
    uint64_t start = s->in.total_out;
    uint32_t want = len > s->in.count ? s->in.count : (uint32_t)len;
    if (s->in.npend && rfds) {
        uint32_t pos0 = s->in.pend[0].pos;                               // never read across the start of a later fd batch
        if (pos0 > (uint32_t)start && pos0 - (uint32_t)start < want) want = pos0 - (uint32_t)start;
        else if (pos0 == (uint32_t)start) {
            *nrfds = s->in.pend[0].fds.n;
            for (int k = 0; k < *nrfds; k++) rfds[k] = s->in.pend[0].fds.f[k];
            for (uint32_t i = 1; i < s->in.npend; i++) s->in.pend[i - 1] = s->in.pend[i];
            s->in.npend--;
        }
    }
    uint32_t left = want;
    uint32_t rd = s->in.rd;
    while (left) {
        uint32_t k = left > sizeof buf ? sizeof buf : left;
        if (peek) { for (uint32_t i = 0; i < k; i++) { buf[i] = s->in.buf[rd]; rd = (rd + 1) % SOCK_BUF; } }
        else ring_read(&s->in, buf, k);
        if (lx_copy_out(t, uaddr + (uint64_t)done, buf, k)) return done ? done : -LX_EFAULT;
        done += k; left -= k;
    }
    return done;
}

int64_t lx_sock_shutdown(struct lx_task *t, int fd, int how) {
    struct lx_sock *s = sock_of(t, fd);
    if (!s) return -LX_EBADF;
    if (how == 0 || how == 2) s->shut_rd = 1;
    if (how == 1 || how == 2) { s->shut_wr = 1; if (s->peer) s->peer->shut_rd = 1; }
    return 0;
}

// getsockname / getpeername: only the family matters to most programs
int64_t lx_sock_name(struct lx_task *t, int fd, uint64_t uaddr, uint64_t ulen, int peer) {
    struct lx_sock *s = sock_of(t, fd);
    if (!s) return -LX_EBADF;
    struct lx_sock *o = peer ? s->peer : s;
    if (peer && !o) return -107;                                         // ENOTCONN
    uint8_t sa[110];
    memset(sa, 0, sizeof sa);
    uint16_t fam = (uint16_t)s->domain; memcpy(sa, &fam, 2);
    uint32_t len = 2;
    if (s->domain == AF_UNIX && o && o->path[0]) {
        uint32_t pl = (uint32_t)strlen(o->path);
        if (o->abstract) { memcpy(sa + 3, o->path, pl); len = 3 + pl; } else { memcpy(sa + 2, o->path, pl + 1); len = 2 + pl + 1; }
    } else if (s->domain == AF_INET) {
        len = 16;
        uint16_t port = swap16(peer ? s->rport : s->lport);
        memcpy(sa + 2, &port, 2);
        if (peer) { sa[4] = (uint8_t)(s->rip >> 24); sa[5] = (uint8_t)(s->rip >> 16); sa[6] = (uint8_t)(s->rip >> 8); sa[7] = (uint8_t)s->rip; }
        else { uint8_t *ip = net_get_ip(); for (int i = 0; i < 4; i++) sa[4 + i] = ip[i]; }
    }
    uint32_t cap;
    if (lx_copy_in(t, &cap, ulen, 4)) return -LX_EFAULT;
    if (lx_copy_out(t, uaddr, sa, cap < len ? cap : len)) return -LX_EFAULT;
    return lx_copy_out(t, ulen, &len, 4);
}

void lx_sock_close(struct lx_sock *s) {
    s->state = 4;
    if (s->peer) {
        s->peer->shut_rd = 1;
        s->peer->peer = s->peer->peer == s ? 0 : s->peer->peer;
        s->peer = 0;
    }
    lx_sock_put(s);
}

// sendmsg: the iovec was gathered into a kernel buffer
int64_t lx_sock_send_kernel(struct lx_task *t, int fd, const uint8_t *buf, uint64_t len, struct lx_file **fds, int nfds, uint64_t to_addr, uint64_t to_len) {
    g_kbuf = buf ? buf : (const uint8_t*)"";
    int64_t r = lx_sock_send(t, fd, 0, len, fds, nfds, to_addr, to_len);
    g_kbuf = 0;
    return r;
}

// recvmsg: fill the iovec in order from one stream; a datagram goes into the first buffer
int64_t lx_sock_recv_iov(struct lx_task *t, int fd, uint64_t *iov, uint64_t iovlen, struct lx_file **rfds, int *nrfds, int peek) {
    struct lx_file *f = (fd >= 0 && fd < LX_MAX_FDS) ? t->files->fds[fd] : 0;
    if (!f || f->kind != LXF_SOCK) return -LX_EBADF;
    int64_t total = 0;
    int dgram = f->sock->type != SOCK_STREAM;
    for (uint64_t i = 0; i < iovlen; i++) {
        uint64_t base = iov[i * 2], len = iov[i * 2 + 1];
        if (!len) continue;
        int64_t r = lx_sock_recv(t, fd, base, len, i == 0 ? rfds : 0, i == 0 ? nrfds : 0, peek);
        if (r < 0) return total ? total : r;
        if (t->state == LXT_BLOCK) return total ? total : 0;
        total += r;
        if (dgram || (uint64_t)r < len || f->sock->in.count == 0) break;
    }
    return total;
}

int lx_sock_hup(struct lx_sock *s) { return s->state == 4 || (s->state == 3 && !s->peer && s->domain == AF_UNIX); }

// ---- AF_INET: TCP clients through the kernel stack, UDP through bound ports -----------------------------------------------

struct sockaddr_in_k { uint16_t family; uint16_t port; uint8_t addr[4]; uint8_t zero[8]; };

static uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static int parse_in(struct lx_task *t, uint64_t uaddr, uint64_t alen, uint32_t *ip, uint16_t *port) {
    struct sockaddr_in_k sa;
    if (alen < 8) return -LX_EINVAL;
    if (lx_copy_in(t, &sa, uaddr, alen < sizeof sa ? alen : sizeof sa)) return -LX_EFAULT;
    if (sa.family != AF_INET) return -97;
    *ip = ((uint32_t)sa.addr[0] << 24) | ((uint32_t)sa.addr[1] << 16) | ((uint32_t)sa.addr[2] << 8) | sa.addr[3];
    *port = swap16(sa.port);
    return 0;
}

int lx_inet_readable(struct lx_sock *s) {
    if (s->tcp < 0) return 0;
    return tcp_rx_avail(s->tcp) > 0 || tcp_failed(s->tcp) || tcp_state(s->tcp) == TCP_STATE_CLOSED;
}

static int connect_done(struct lx_task *t) {
    struct lx_sock *s = sock_of(t, (int64_t)t->block_arg);
    if (!s || s->tcp < 0) return 1;
    int st = tcp_state(s->tcp);
    return st == TCP_STATE_ESTABLISHED || tcp_failed(s->tcp) || st == TCP_STATE_CLOSED;
}

// udp receive: queue [len(4) ip(4) port(2) pad(2)] + payload in the socket's ring
static void udp_deliver(void *ctx, uint32_t sip, uint16_t sport, const uint8_t *data, uint32_t len) {
    struct lx_sock *s = (struct lx_sock*)ctx;
    if (len > 1500 || s->in.count + 12 + len > SOCK_BUF) return;
    uint8_t hdr[12];
    memcpy(hdr, &len, 4); memcpy(hdr + 4, &sip, 4); memcpy(hdr + 8, &sport, 2); hdr[10] = hdr[11] = 0;
    ring_write(&s->in, hdr, 12);
    ring_write(&s->in, data, len);
}

static int udp_ensure_bound(struct lx_sock *s) {
    if (s->udp_bound) return 0;
    static uint16_t next_port = 40000;
    for (int tries = 0; tries < 64; tries++) {
        uint16_t p = s->lport ? s->lport : next_port++;
        if (next_port > 60000) next_port = 40000;
        if (udp_bind_port(p, udp_deliver, s) == 0) { s->lport = p; s->udp_bound = 1; return 0; }
        if (s->lport) return -98;
    }
    return -98;
}

int64_t lx_inet_connect(struct lx_task *t, struct lx_sock *s, uint64_t uaddr, uint64_t alen) {
    uint32_t ip; uint16_t port;
    if (s->type == SOCK_DGRAM) {
        int r = parse_in(t, uaddr, alen, &ip, &port);
        if (r) return r;
        s->rip = ip; s->rport = port; s->state = 3;
        return udp_ensure_bound(s);
    }
    int fd = -1;
    for (int i = 0; i < LX_MAX_FDS; i++) { struct lx_file *f = t->files->fds[i]; if (f && f->kind == LXF_SOCK && f->sock == s) { fd = i; break; } }
    if (s->tcp < 0) {
        int r = parse_in(t, uaddr, alen, &ip, &port);
        if (r) return r;
        s->rip = ip; s->rport = port;
        s->tcp = tcp_open(ip, port);
        if (s->tcp < 0) return -101;                                     // ENETUNREACH
        s->state = 5;
        if (t->files->fds[fd]->flags & LX_O_NONBLOCK) return -115;       // EINPROGRESS
    }
    if (s->state == 3) return 0;
    if (tcp_failed(s->tcp) || tcp_state(s->tcp) == TCP_STATE_CLOSED) { tcp_abort(s->tcp); s->tcp = -1; s->state = 0; return -111; }
    if (tcp_state(s->tcp) == TCP_STATE_ESTABLISHED) { s->state = 3; return 0; }
    lx_block(t, connect_done, (uint64_t)fd);
    return 0;
}

int64_t lx_inet_accept(struct lx_task *t, struct lx_sock *s, int flags) { (void)t; (void)s; (void)flags; return -95; }   // EOPNOTSUPP: no TCP servers yet

static int send_ready(struct lx_task *t) {
    struct lx_sock *s = sock_of(t, (int64_t)t->block_arg);
    return !s || s->tcp < 0 || tcp_send_space(s->tcp) > 0 || tcp_failed(s->tcp);
}

int64_t lx_inet_send(struct lx_task *t, struct lx_file *f, struct lx_sock *s, uint64_t uaddr, uint64_t len, uint64_t to_addr, uint64_t to_len) {
    int fd = -1;
    for (int i = 0; i < LX_MAX_FDS; i++) if (t->files->fds[i] == f) { fd = i; break; }
    uint8_t buf[1500];
    if (s->type == SOCK_DGRAM) {
        uint32_t ip = s->rip; uint16_t port = s->rport;
        if (to_addr) { int r = parse_in(t, to_addr, to_len, &ip, &port); if (r) return r; }
        else if (s->state != 3) return -89;                              // EDESTADDRREQ
        if (len > 1472) return -90;
        int r = udp_ensure_bound(s);
        if (r) return r;
        if (lx_copy_in(t, buf, uaddr, len)) return -LX_EFAULT;
        uint8_t dst[4] = { (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip };
        udp_send(dst, s->lport, port, buf, (uint16_t)len);
        return (int64_t)len;
    }
    if (s->tcp < 0 || s->state != 3) return -107;                        // ENOTCONN
    if (tcp_failed(s->tcp)) { lx_signal(t, 13); return -LX_EPIPE; }
    int64_t done = 0;
    while (len) {
        uint32_t room = tcp_send_space(s->tcp);
        if (!room) break;
        uint32_t k = len > sizeof buf ? (uint32_t)sizeof buf : (uint32_t)len;
        if (k > room) k = room;
        if (lx_copy_in(t, buf, uaddr, k)) return done ? done : -LX_EFAULT;
        if (tcp_send(s->tcp, buf, k) != 0) break;
        uaddr += k; len -= k; done += k;
    }
    if (!done && len) {
        if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
        lx_block(t, send_ready, (uint64_t)fd);
        return 0;
    }
    return done;
}

static int recv_ready(struct lx_task *t) {
    struct lx_sock *s = sock_of(t, (int64_t)t->block_arg);
    return !s || lx_sock_readable(s);
}

int64_t lx_inet_recv(struct lx_task *t, struct lx_file *f, struct lx_sock *s, uint64_t uaddr, uint64_t len) {
    int fd = -1;
    for (int i = 0; i < LX_MAX_FDS; i++) if (t->files->fds[i] == f) { fd = i; break; }
    uint8_t buf[1500];
    if (s->type == SOCK_DGRAM) {
        if (s->in.count < 12) {
            if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
            lx_block(t, recv_ready, (uint64_t)fd);
            return 0;
        }
        uint8_t hdr[12];
        ring_read(&s->in, hdr, 12);
        uint32_t mlen; memcpy(&mlen, hdr, 4);
        uint32_t n = mlen < len ? mlen : (uint32_t)len;
        ring_read(&s->in, buf, mlen);
        if (lx_copy_out(t, uaddr, buf, n)) return -LX_EFAULT;
        return n;
    }
    if (s->tcp < 0 || s->state != 3) return -107;
    uint32_t cap = len > sizeof buf ? (uint32_t)sizeof buf : (uint32_t)len;
    int n = tcp_recv(s->tcp, buf, cap);
    if (n < 0) return 0;                                                 // end of stream
    if (n == 0) {
        if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
        lx_block(t, recv_ready, (uint64_t)fd);
        return 0;
    }
    if (lx_copy_out(t, uaddr, buf, (uint64_t)n)) return -LX_EFAULT;
    return n;
}
