// Linux x86-64 system calls of the Linux subsystem.
#include "lx.h"
#include "crypto/rand.h"
#include "memory.h"
#include "paging.h"
#include "serial.h"
#include "string.h"
#include "window.h"

extern int64_t kclock_epoch_ms(void);
extern struct lx_task *lx_fork(struct lx_task *p);
extern void lx_reap(struct lx_task *t);
extern void lx_signal(struct lx_task *t, int sig);
extern int lx_task_by_pid(int pid, struct lx_task **out);
extern int lx_has_child(struct lx_task *p, int pid);
extern struct lx_task *lx_find_zombie_child(struct lx_task *p, int pid);
extern uint64_t lx_now_ns(void);
extern int lx_tty_read_ready(struct lx_task *t);
extern int lx_tty_read(struct lx_task *t, uint8_t *buf, uint32_t len);
extern void lx_tty_write(struct lx_task *t, const uint8_t *buf, uint32_t len);
extern int lx_tty_ioctl(struct lx_task *t, uint64_t req, uint64_t arg);
extern int lx_fb_open(struct lx_task *t);
extern int64_t lx_syscall_more(struct lx_task *t, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

#define AT_FDCWD (-100)

// ---- files ------------------------------------------------------------------------------------------------------

struct lx_file *lx_file_new(int kind, int flags, struct lx_node *node) {
    struct lx_file *f = (struct lx_file*)kcalloc(1, sizeof *f);
    if (!f) return 0;
    f->refs = 1;
    f->kind = kind;
    f->flags = flags;
    f->node = node;
    if (node) lxfs_ref(node);
    return f;
}

void lx_file_put(struct lx_file *f) {
    if (--f->refs > 0) return;
    if (f->node) lxfs_unref(f->node);
    if (f->sock) lx_sock_close(f->sock);
    if (f->ev) lx_event_free(f->ev);
    if (f->priv) { extern void lx_okwin_close(void*); lx_okwin_close(f->priv); }
    if (f->pipe) {
        if (f->kind == LXF_PIPE_R) f->pipe->readers--;
        else if (f->kind == LXF_PIPE_W) f->pipe->writers--;
        if (f->pipe->readers <= 0 && f->pipe->writers <= 0) { kfree(f->pipe->buf); kfree(f->pipe); }
    }
    kfree(f);
}

int lx_fd_alloc(struct lx_task *t, struct lx_file *f, int minfd, int cloexec) {
    for (int i = minfd; i < LX_MAX_FDS; i++)
        if (!t->files->fds[i]) { t->files->fds[i] = f; t->files->fdflags[i] = cloexec ? 1 : 0; return i; }
    return -LX_EMFILE;
}

static struct lx_file *fd_get(struct lx_task *t, int64_t fd) {
    if (fd < 0 || fd >= LX_MAX_FDS) return 0;
    return t->files->fds[fd];
}

// ---- pipes ------------------------------------------------------------------------------------------------------

static int pipe_new(struct lx_file **r, struct lx_file **w) {
    struct lx_pipe *p = (struct lx_pipe*)kcalloc(1, sizeof *p);
    if (!p) return -LX_ENOMEM;
    p->buf = (uint8_t*)kmalloc(LX_PIPE_SIZE);
    if (!p->buf) { kfree(p); return -LX_ENOMEM; }
    *r = lx_file_new(LXF_PIPE_R, LX_O_RDONLY, 0);
    *w = lx_file_new(LXF_PIPE_W, LX_O_WRONLY, 0);
    if (!*r || !*w) return -LX_ENOMEM;
    (*r)->pipe = (*w)->pipe = p;
    p->readers = p->writers = 1;
    return 0;
}

static int pipe_can_read(struct lx_task *t) {
    struct lx_file *f = fd_get(t, (int64_t)t->block_arg);
    return !f || !f->pipe || f->pipe->count > 0 || f->pipe->writers <= 0;
}
static int pipe_can_write(struct lx_task *t) {
    struct lx_file *f = fd_get(t, (int64_t)t->block_arg);
    return !f || !f->pipe || f->pipe->count < LX_PIPE_SIZE || f->pipe->readers <= 0;
}

// ---- character devices ------------------------------------------------------------------------------------------

static void random_fill(uint8_t *b, uint32_t n) {
    if (rand_ready() && rand_bytes(b, n)) return;
    static uint64_t s;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    s ^= ((uint64_t)hi << 32 | lo) | 1;
    for (uint32_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; b[i] = (uint8_t)(s >> 24); }
}

// ---- reading and writing a file object ----------------------------------------------------------------------------

#define IOBUF 4096

static int fd_of(struct lx_task *t, struct lx_file *f) {
    for (int i = 0; i < LX_MAX_FDS; i++) if (t->files->fds[i] == f) return i;
    return -1;
}

static int64_t file_read(struct lx_task *t, struct lx_file *f, uint64_t uaddr, uint64_t len, int use_off, uint64_t off) {
    uint8_t buf[IOBUF];
    if (len > 0x7FFFF000ull) len = 0x7FFFF000ull;
    int64_t done = 0;
    switch (f->kind) {
    case LXF_TTY: {
        if ((f->flags & LX_O_ACCMODE) == LX_O_WRONLY) return -LX_EBADF;
        if (!len) return 0;
        if (!lx_tty_read_ready(t)) {
            if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
            extern int lx_tty_wake(struct lx_task*);
            lx_block(t, lx_tty_wake, 0);
            return 0;
        }
        uint32_t n = len > IOBUF ? IOBUF : (uint32_t)len;
        int got = lx_tty_read(t, buf, n);
        if (got > 0 && lx_copy_out(t, uaddr, buf, (uint64_t)got)) return -LX_EFAULT;
        return got;
    }
    case LXF_PIPE_R: {
        struct lx_pipe *p = f->pipe;
        if (!len) return 0;
        if (p->count == 0) {
            if (p->writers <= 0) return 0;
            if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
            int fd = -1;
            for (int i = 0; i < LX_MAX_FDS; i++) if (t->files->fds[i] == f) { fd = i; break; }
            lx_block(t, pipe_can_read, (uint64_t)fd);
            return 0;
        }
        while (len && p->count) {
            uint32_t n = p->count < IOBUF ? p->count : IOBUF;
            if (n > len) n = (uint32_t)len;
            if (n > LX_PIPE_SIZE - p->rd) n = LX_PIPE_SIZE - p->rd;
            if (lx_copy_out(t, uaddr, p->buf + p->rd, n)) return done ? done : -LX_EFAULT;
            p->rd = (p->rd + n) % LX_PIPE_SIZE; p->count -= n;
            uaddr += n; len -= n; done += n;
        }
        return done;
    }
    case LXF_SOCK: {
        extern int64_t lx_sock_recv(struct lx_task*, int, uint64_t, uint64_t, struct lx_file**, int*, int);
        return lx_sock_recv(t, fd_of(t, f), uaddr, len, 0, 0, 0);
    }
    case LXF_EVENT: {
        extern int64_t lx_event_read(struct lx_task*, struct lx_file*, uint64_t, uint64_t);
        return lx_event_read(t, f, uaddr, len);
    }
    case LXF_NODE: {
        struct lx_node *n = f->node;
        if ((f->flags & LX_O_ACCMODE) == LX_O_WRONLY) return -LX_EBADF;
        if (n->type == LXN_DIR) return -LX_EISDIR;
        if (n->type == LXN_CHR) {
            uint32_t dev = n->dev;
            if (dev == LXDEV_NULL) return 0;
            if (dev == LXDEV_ZERO || dev == LXDEV_FULL || dev == LXDEV_RANDOM || dev == LXDEV_URANDOM) {
                while (len) {
                    uint32_t k = len > IOBUF ? IOBUF : (uint32_t)len;
                    if (dev == LXDEV_ZERO || dev == LXDEV_FULL) memset(buf, 0, k); else random_fill(buf, k);
                    if (lx_copy_out(t, uaddr, buf, k)) return done ? done : -LX_EFAULT;
                    uaddr += k; len -= k; done += k;
                }
                return done;
            }
            if (dev == LXDEV_OKWIN) {
                extern int lx_okwin_ready(void*); extern int64_t lx_okwin_read(struct lx_task*, void*, uint64_t, uint64_t);
                extern int lx_okwin_wake(struct lx_task*);
                if (!lx_okwin_ready(f->priv)) {
                    if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
                    lx_block(t, lx_okwin_wake, (uint64_t)fd_of(t, f));
                    return 0;
                }
                return lx_okwin_read(t, f->priv, uaddr, len);
            }
            if (dev == LXDEV_EVENT0) {
                extern int lx_input_ready(void); extern int lx_input_read(struct lx_task*, uint64_t, uint64_t); extern int lx_input_wake(struct lx_task*);
                if (!lx_input_ready()) {
                    if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
                    lx_block(t, lx_input_wake, 0);
                    return 0;
                }
                return lx_input_read(t, uaddr, len);
            }
            if (dev == LXDEV_TTY || dev == LXDEV_CONSOLE) {
                if (!len) return 0;
                if (!lx_tty_read_ready(t)) {
                    if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
                    extern int lx_tty_wake(struct lx_task*);
                    lx_block(t, lx_tty_wake, 0);
                    return 0;
                }
                int got = lx_tty_read(t, buf, len > IOBUF ? IOBUF : (uint32_t)len);
                if (got > 0 && lx_copy_out(t, uaddr, buf, (uint64_t)got)) return -LX_EFAULT;
                return got;
            }
            return -LX_EIO;
        }
        uint64_t o = use_off ? off : f->off;
        while (len) {
            uint32_t k = len > IOBUF ? IOBUF : (uint32_t)len;
            int64_t r = lxfs_read(n, o, buf, k);
            if (r <= 0) break;
            if (lx_copy_out(t, uaddr, buf, (uint64_t)r)) return done ? done : -LX_EFAULT;
            uaddr += (uint64_t)r; len -= (uint64_t)r; done += r; o += (uint64_t)r;
            if (r < k) break;
        }
        if (!use_off) f->off = o;
        return done;
    }
    default: return -LX_EBADF;
    }
}

static int64_t file_write(struct lx_task *t, struct lx_file *f, uint64_t uaddr, uint64_t len, int use_off, uint64_t off) {
    uint8_t buf[IOBUF];
    if (len > 0x7FFFF000ull) len = 0x7FFFF000ull;
    int64_t done = 0;
    switch (f->kind) {
    case LXF_TTY:
        if ((f->flags & LX_O_ACCMODE) == LX_O_RDONLY && 0) return -LX_EBADF;
        while (len) {
            uint32_t k = len > IOBUF ? IOBUF : (uint32_t)len;
            if (lx_copy_in(t, buf, uaddr, k)) return done ? done : -LX_EFAULT;
            lx_tty_write(t, buf, k);
            uaddr += k; len -= k; done += k;
        }
        return done;
    case LXF_PIPE_W: {
        struct lx_pipe *p = f->pipe;
        if (p->readers <= 0) { lx_signal(t, 13); return -LX_EPIPE; }
        while (len) {
            if (p->count == LX_PIPE_SIZE) {
                if (done) return done;
                if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
                int fd = -1;
                for (int i = 0; i < LX_MAX_FDS; i++) if (t->files->fds[i] == f) { fd = i; break; }
                lx_block(t, pipe_can_write, (uint64_t)fd);
                return 0;
            }
            uint32_t k = LX_PIPE_SIZE - p->count;
            if (k > len) k = (uint32_t)len;
            if (k > IOBUF) k = IOBUF;
            if (k > LX_PIPE_SIZE - p->wr) k = LX_PIPE_SIZE - p->wr;
            if (lx_copy_in(t, p->buf + p->wr, uaddr, k)) return done ? done : -LX_EFAULT;
            p->wr = (p->wr + k) % LX_PIPE_SIZE; p->count += k;
            uaddr += k; len -= k; done += k;
        }
        return done;
    }
    case LXF_SOCK: {
        extern int64_t lx_sock_send(struct lx_task*, int, uint64_t, uint64_t, struct lx_file**, int, uint64_t, uint64_t);
        return lx_sock_send(t, fd_of(t, f), uaddr, len, 0, 0, 0, 0);
    }
    case LXF_EVENT: {
        extern int64_t lx_event_write(struct lx_task*, struct lx_file*, uint64_t, uint64_t);
        return lx_event_write(t, f, uaddr, len);
    }
    case LXF_NODE: {
        struct lx_node *n = f->node;
        if ((f->flags & LX_O_ACCMODE) == LX_O_RDONLY) return -LX_EBADF;
        if (n->type == LXN_DIR) return -LX_EISDIR;
        if (n->type == LXN_CHR) {
            uint32_t dev = n->dev;
            if (dev == LXDEV_NULL || dev == LXDEV_ZERO || dev == LXDEV_RANDOM || dev == LXDEV_URANDOM) return (int64_t)len;
            if (dev == LXDEV_FULL) return -LX_ENOSPC;
            if (dev == LXDEV_TTY || dev == LXDEV_CONSOLE) {
                while (len) {
                    uint32_t k = len > IOBUF ? IOBUF : (uint32_t)len;
                    if (lx_copy_in(t, buf, uaddr, k)) return done ? done : -LX_EFAULT;
                    lx_tty_write(t, buf, k);
                    uaddr += k; len -= k; done += k;
                }
                return done;
            }
            return -LX_EIO;
        }
        uint64_t o = use_off ? off : ((f->flags & LX_O_APPEND) ? n->size : f->off);
        while (len) {
            uint32_t k = len > IOBUF ? IOBUF : (uint32_t)len;
            if (lx_copy_in(t, buf, uaddr, k)) return done ? done : -LX_EFAULT;
            int64_t r = lxfs_write(n, o, buf, k);
            if (r < 0) return done ? done : r;
            uaddr += k; len -= k; done += k; o += k;
        }
        if (!use_off) f->off = o;
        return done;
    }
    default: return -LX_EBADF;
    }
}

// ---- paths ------------------------------------------------------------------------------------------------------

// Resolve a user path relative to dirfd. 0 = failure with *err set.
static struct lx_node *path_node(struct lx_task *t, int64_t dirfd, uint64_t upath, int follow, int *err) {
    char path[LX_PATH_MAX];
    int n = lx_copy_str(t, path, upath, sizeof path);
    if (n < 0) { *err = n; return 0; }
    if (n == 0) { *err = -LX_ENOENT; return 0; }
    struct lx_node *base = t->cwd;
    if (path[0] != '/' && (int32_t)dirfd != AT_FDCWD) {
        struct lx_file *df = fd_get(t, (int32_t)dirfd);
        if (!df || !df->node || df->node->type != LXN_DIR) { *err = -LX_EBADF; return 0; }
        base = df->node;
    }
    return lxfs_resolve(base, path, follow, err);
}

static int path_parent(struct lx_task *t, int64_t dirfd, uint64_t upath, struct lx_node **dir, char *name) {
    char path[LX_PATH_MAX];
    int n = lx_copy_str(t, path, upath, sizeof path);
    if (n < 0) return n;
    if (n == 0) return -LX_ENOENT;
    struct lx_node *base = t->cwd;
    if (path[0] != '/' && (int32_t)dirfd != AT_FDCWD) {
        struct lx_file *df = fd_get(t, (int32_t)dirfd);
        if (!df || !df->node || df->node->type != LXN_DIR) return -LX_EBADF;
        base = df->node;
    }
    return lxfs_resolve_parent(base, path, dir, name);
}

// ---- stat -------------------------------------------------------------------------------------------------------

struct lx_stat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t size, blksize, blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    int64_t unused[3];
};

static uint32_t node_mode(struct lx_node *n) {
    uint32_t fmt = n->type == LXN_DIR ? 0040000 : n->type == LXN_CHR ? 0020000 : n->type == LXN_LNK ? 0120000 : n->type == LXN_SOCK ? 0140000 : 0100000;
    return fmt | n->mode;
}

static void stat_fill(struct lx_stat *st, struct lx_node *n) {
    memset(st, 0, sizeof *st);
    st->dev = 1; st->ino = n->ino; st->nlink = n->nlink ? n->nlink : 1;
    st->mode = node_mode(n); st->uid = n->uid; st->gid = n->gid;
    st->rdev = n->type == LXN_CHR ? n->dev : 0;
    st->size = (int64_t)n->size; st->blksize = 4096; st->blocks = (int64_t)((n->size + 511) / 512);
    st->atime = st->mtime = st->ctime = n->mtime;
}

static int64_t do_stat_node(struct lx_task *t, struct lx_node *n, uint64_t ubuf) {
    struct lx_stat st;
    stat_fill(&st, n);
    return lx_copy_out(t, ubuf, &st, sizeof st);
}

static int64_t do_stat_file(struct lx_task *t, struct lx_file *f, uint64_t ubuf) {
    if (f->node) return do_stat_node(t, f->node, ubuf);
    struct lx_stat st;
    memset(&st, 0, sizeof st);
    st.dev = 1; st.nlink = 1; st.blksize = 4096;
    st.mode = (f->kind == LXF_TTY) ? 0020620 : 0010600;          // tty, or a FIFO for pipes
    st.rdev = f->kind == LXF_TTY ? LXDEV_TTY : 0;
    return lx_copy_out(t, ubuf, &st, sizeof st);
}

// ---- open -------------------------------------------------------------------------------------------------------

static int64_t do_openat(struct lx_task *t, int64_t dirfd, uint64_t upath, uint32_t flags, uint32_t mode) {
    int err = 0;
    struct lx_node *n = path_node(t, dirfd, upath, !(flags & LX_O_CREAT && flags & LX_O_EXCL), &err);
    if (!n && err == -LX_ENOENT && (flags & LX_O_CREAT)) {
        struct lx_node *dir; char name[LX_NAME_MAX + 1];
        int r = path_parent(t, dirfd, upath, &dir, name);
        if (r) return r;
        n = lxfs_create(dir, name, LXN_REG, mode & ~t->umask & 07777, &err);
        if (!n) return err;
        n->uid = 0;
    } else if (!n) return err;
    else if ((flags & LX_O_CREAT) && (flags & LX_O_EXCL)) return -LX_EEXIST;
    if (n->type == LXN_LNK) return -LX_ELOOP;
    if ((flags & LX_O_DIRECTORY) && n->type != LXN_DIR) return -LX_ENOTDIR;
    if (n->type == LXN_DIR && (flags & LX_O_ACCMODE) != LX_O_RDONLY) return -LX_EISDIR;
    if ((flags & LX_O_TRUNC) && n->type == LXN_REG && (flags & LX_O_ACCMODE) != LX_O_RDONLY) lxfs_truncate(n, 0);
    struct lx_file *f;
    if (n->type == LXN_CHR && (n->dev == LXDEV_FB0 || n->dev == LXDEV_EVENT0)) {
        extern int lx_fb_open_hook(struct lx_task*);
        int hr = lx_fb_open_hook(t);
        if (hr) return hr;
    }
    f = lx_file_new(LXF_NODE, (int)flags, n);
    if (!f) return -LX_ENOMEM;
    if (n->type == LXN_CHR && n->dev == LXDEV_OKWIN) {
        extern void *lx_okwin_open(struct lx_task*);
        f->priv = lx_okwin_open(t);
        if (!f->priv) { lx_file_put(f); return -LX_ENOMEM; }
    }
    int fd = lx_fd_alloc(t, f, 0, (flags & LX_O_CLOEXEC) != 0);
    if (fd < 0) lx_file_put(f);
    return fd;
}

// ---- memory -----------------------------------------------------------------------------------------------------

static int64_t do_mmap(struct lx_task *t, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, int64_t fd, uint64_t off) {
    if (!len || len > 0x40000000ull) return -LX_ENOMEM;
    uint32_t l = (uint32_t)((len + 0xFFF) & ~0xFFFull);
    struct lx_node *node = 0;
    if (!(flags & LX_MAP_ANON)) {
        struct lx_file *f = fd_get(t, fd);
        if (!f) return -LX_EBADF;
        if (!f->node) return -LX_ENODEV;
        if (f->node->type == LXN_CHR && f->node->dev == LXDEV_FB0) {
            extern int64_t lx_fb_mmap(struct lx_task*, uint32_t len);
            return lx_fb_mmap(t, l);
        }
        if (f->node->type != LXN_REG) return -LX_ENODEV;
        if (off & 0xFFF) return -LX_EINVAL;
        node = f->node;
    }
    uint32_t start;
    if (flags & LX_MAP_FIXED) {
        if (addr & 0xFFF) return -LX_EINVAL;
        start = (uint32_t)addr;
        if (addr < LX_USER_BASE || addr + l > LX_USER_TOP) return -LX_ENOMEM;
    } else {
        start = 0;
        if (addr && !(addr & 0xFFF) && addr >= LX_USER_BASE && addr + l <= LX_USER_TOP) {
            int free = 1;
            for (int i = 0; i < t->mm->nvma; i++) if (addr < t->mm->vma[i].end && addr + l > t->mm->vma[i].start) { free = 0; break; }
            if (free) start = (uint32_t)addr;
        }
        if (!start) start = lx_find_free(t, l);
        if (!start) return -LX_ENOMEM;
    }
    int r = lx_map_region(t, start, l, prot, flags, node, off);
    if (r) return r;
    if (node) { struct lx_vma *v = lx_vma_find(t, start); if (v) v->fend = start + l; }
    if (!(flags & LX_MAP_FIXED) && (!addr)) t->mm->mmap_hint = start;
    return start;
}

static int64_t do_brk(struct lx_task *t, uint64_t req) {
    if (req == 0 || req < t->mm->brk_start) return t->mm->brk;
    if (req >= LX_MMAP_TOP - (64u << 20)) return t->mm->brk;
    uint32_t nb = (uint32_t)req;
    uint32_t old_end = (t->mm->brk + 0xFFF) & ~0xFFFu, new_end = (nb + 0xFFF) & ~0xFFFu;
    if (new_end > old_end) {
        for (int i = 0; i < t->mm->nvma; i++)
            if (old_end < t->mm->vma[i].end && new_end > t->mm->vma[i].start && !(t->mm->vma[i].start == t->mm->brk_start && 0)) return t->mm->brk;
        if (lx_map_region(t, old_end, new_end - old_end, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANON, 0, 0)) return t->mm->brk;
    } else if (new_end < old_end) {
        lx_unmap_region(t, new_end, old_end - new_end);
    }
    t->mm->brk = nb;
    return t->mm->brk;
}

// ---- time -------------------------------------------------------------------------------------------------------

struct lx_timespec { int64_t sec, nsec; };

static void clock_get(int clk, struct lx_timespec *ts) {
    if (clk == 0 || clk == 5 || clk == 8) {                    // REALTIME
        int64_t ms = kclock_epoch_ms();
        uint64_t sub = lx_now_ns() % 1000000ull;
        ts->sec = ms / 1000; ts->nsec = (ms % 1000) * 1000000 + (int64_t)sub;
        if (ts->nsec >= 1000000000) { ts->sec++; ts->nsec -= 1000000000; }
    } else {
        uint64_t ns = lx_now_ns();
        ts->sec = (int64_t)(ns / 1000000000ull); ts->nsec = (int64_t)(ns % 1000000000ull);
    }
}

static int sleep_done(struct lx_task *t) { return lx_now_ms() >= (int64_t)t->wake_ms; }

static int64_t do_sleep(struct lx_task *t, uint64_t ureq) {
    if (t->wake_ms == 0) {
        struct lx_timespec ts;
        if (lx_copy_in(t, &ts, ureq, sizeof ts)) return -LX_EFAULT;
        if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000) return -LX_EINVAL;
        int64_t ms = ts.sec * 1000 + (ts.nsec + 999999) / 1000000;
        t->wake_ms = (uint64_t)(lx_now_ms() + ms);
        if (ms == 0) { t->wake_ms = 0; return 0; }
    }
    if (lx_now_ms() >= (int64_t)t->wake_ms) { t->wake_ms = 0; return 0; }
    lx_block(t, sleep_done, 0);
    return 0;
}

// ---- futex wait queues, vfork, signals -----------------------------------------------------------------------------

extern struct lx_task *lx_task_at(int i);
extern void lx_signal_group(int tgid, int sig);
extern struct lx_task *lx_clone(struct lx_task *p, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t ctid, uint64_t tls);
extern void lx_thread_exit(struct lx_task *t, int status);
extern int64_t lx_sys_rt_sigreturn(struct lx_task *t);
extern int64_t lx_sys_alarm(struct lx_task *t, uint64_t secs);
extern int64_t lx_sys_setitimer(struct lx_task *t, uint64_t which, uint64_t unew, uint64_t uold);
extern int64_t lx_sys_getitimer(struct lx_task *t, uint64_t which, uint64_t uval);

static int futex_ready(struct lx_task *t) {
    if (t->futex_state == 2) return 1;
    if (t->wake_ms && lx_now_ms() >= (int64_t)t->wake_ms) { t->futex_state = 3; return 1; }
    return 0;
}

static int sig_wait_ready(struct lx_task *t) { return (t->sig_pending & ~t->sig_blocked) != 0; }

static int vfork_ready(struct lx_task *t) {
    struct lx_task *c = 0;
    if (!lx_task_by_pid((int)t->block_arg, &c)) return 1;
    return c->execd || c->state == LXT_ZOMBIE || c->state == LXT_FREE;
}

int lx_futex_wake(struct lx_mm *mm, uint64_t addr, int n) {
    int woken = 0;
    for (int i = 0; i < LX_MAX_TASKS && woken < n; i++) {
        struct lx_task *w = lx_task_at(i);
        if (w && w->pid && w->state == LXT_BLOCK && w->futex_state == 1 && w->futex_addr == addr && w->mm == mm) { w->futex_state = 2; woken++; }
    }
    return woken;
}

void lx_futex_wake_addr(struct lx_mm *mm, uint64_t addr, int n) { lx_futex_wake(mm, addr, n); }

void lx_futex_requeue(struct lx_mm *mm, uint64_t from, uint64_t to, int n) {
    for (int i = 0; i < LX_MAX_TASKS && n > 0; i++) {
        struct lx_task *w = lx_task_at(i);
        if (w && w->pid && w->state == LXT_BLOCK && w->futex_state == 1 && w->futex_addr == from && w->mm == mm) { w->futex_addr = to; n--; }
    }
}

// ---- processes --------------------------------------------------------------------------------------------------

static int wait_ready(struct lx_task *t) {
    return lx_find_zombie_child(t, (int)(int64_t)t->block_arg) != 0;
}

static int64_t do_wait4(struct lx_task *t, int64_t pid, uint64_t ustatus, uint32_t options) {
    int want = (int)pid;
    if (want < -1 || want == 0) want = -1;                      // process groups: any child
    if (!lx_has_child(t, want)) return -LX_ECHILD;
    struct lx_task *z = lx_find_zombie_child(t, want);
    if (!z) {
        if (options & 1) return 0;                              // WNOHANG
        lx_block(t, wait_ready, (uint64_t)(int64_t)want);
        return 0;
    }
    int ret = z->pid;
    int status = z->exit_code < 128 && z->exit_code >= 0 && (z->exit_code & 0xFF00) == 0 ? z->exit_code : z->exit_code;
    if (ustatus && lx_copy_out(t, ustatus, &status, 4)) return -LX_EFAULT;
    lx_reap(z);
    return ret;
}

static int64_t do_execve(struct lx_task *t, uint64_t upath, uint64_t uargv, uint64_t uenvp) {
    char path[LX_PATH_MAX];
    int n = lx_copy_str(t, path, upath, sizeof path);
    if (n < 0) return n;
    enum { MAXV = 256 };
    char **av = (char**)kcalloc(MAXV + 1, sizeof(char*)), **ev = (char**)kcalloc(MAXV + 1, sizeof(char*));
    int64_t r = 0;
    if (!av || !ev) { r = -LX_ENOMEM; goto out; }
    for (int pass = 0; pass < 2; pass++) {
        uint64_t vec = pass ? uenvp : uargv;
        char **dst = pass ? ev : av;
        for (int i = 0; vec && i < MAXV; i++) {
            uint64_t p;
            if (lx_copy_in(t, &p, vec + (uint64_t)i * 8, 8)) { r = -LX_EFAULT; goto out; }
            if (!p) break;
            char *s = (char*)kmalloc(LX_PATH_MAX);
            if (!s) { r = -LX_ENOMEM; goto out; }
            int sl = lx_copy_str(t, s, p, LX_PATH_MAX);
            if (sl < 0) { kfree(s); r = sl; goto out; }
            dst[i] = s;
        }
    }
    {
        int err = 0;
        struct lx_node *node = lxfs_resolve(t->cwd, path, 1, &err);
        if (!node) { r = err; goto out; }
        r = lx_exec_load(t, path, (const char *const*)av, (const char *const*)ev);
        if (r == 0) {
            for (int i = 0; i < LX_MAX_FDS; i++)
                if (t->files->fds[i] && (t->files->fdflags[i] & 1)) { lx_file_put(t->files->fds[i]); t->files->fds[i] = 0; t->files->fdflags[i] = 0; }
            t->skip_ret = 1;
            t->execd = 1;
        } else if (t->mm->nvma == 0) {                               // the old image is already gone: nothing to return to
            lx_task_exit(t, 127 << 8);
        }
    }
out:
    for (int i = 0; av && i < MAXV; i++) if (av[i]) kfree(av[i]);
    for (int i = 0; ev && i < MAXV; i++) if (ev[i]) kfree(ev[i]);
    if (av) kfree(av);
    if (ev) kfree(ev);
    return r;
}

// ---- directory listing --------------------------------------------------------------------------------------------

static int64_t do_getdents64(struct lx_task *t, struct lx_file *f, uint64_t ubuf, uint64_t len) {
    if (!f->node || f->node->type != LXN_DIR) return -LX_ENOTDIR;
    uint8_t out[4096];
    uint64_t used = 0;
    for (;;) {
        const char *name;
        struct lx_node *c = lxfs_dir_entry(f->node, f->off, &name);
        if (!c) break;
        uint32_t nl = (uint32_t)strlen(name);
        uint32_t rec = (19 + nl + 1 + 7) & ~7u;
        if (used + rec > len || used + rec > sizeof out) {
            if (!used) return -LX_EINVAL;
            break;
        }
        uint8_t *p = out + used;
        memset(p, 0, rec);
        uint64_t ino = c->ino, off = f->off + 1;
        memcpy(p, &ino, 8); memcpy(p + 8, &off, 8);
        p[16] = (uint8_t)rec; p[17] = (uint8_t)(rec >> 8);
        p[18] = c->type == LXN_DIR ? 4 : c->type == LXN_CHR ? 2 : c->type == LXN_LNK ? 10 : c->type == LXN_SOCK ? 12 : 8;
        memcpy(p + 19, name, nl);
        used += rec;
        f->off++;
    }
    if (used && lx_copy_out(t, ubuf, out, used)) return -LX_EFAULT;
    return (int64_t)used;
}

// ---- poll -------------------------------------------------------------------------------------------------------

#define POLLIN 1
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
#define POLLNVAL 32

static int fd_events(struct lx_task *t, struct lx_file *f, int want) {
    int r = 0;
    if (!f) return POLLNVAL;
    switch (f->kind) {
    case LXF_TTY: if (lx_tty_read_ready(t)) r |= POLLIN; r |= POLLOUT; break;
    case LXF_SOCK: { if (lx_sock_readable(f->sock)) r |= POLLIN; if (lx_sock_writable(f->sock)) r |= POLLOUT; if (lx_sock_hup(f->sock)) r |= POLLHUP; break; }
    case LXF_EVENT: r |= lx_event_ready(t, f, want); break;
    case LXF_PIPE_R: if (f->pipe->count) r |= POLLIN; if (f->pipe->writers <= 0) r |= POLLHUP; break;
    case LXF_PIPE_W: if (f->pipe->count < LX_PIPE_SIZE) r |= POLLOUT; if (f->pipe->readers <= 0) r |= POLLERR; break;
    case LXF_NODE:
        if (f->node && f->node->type == LXN_CHR && f->node->dev == LXDEV_EVENT0) { extern int lx_input_ready(void); if (lx_input_ready()) r |= POLLIN; break; }
        if (f->node && f->node->type == LXN_CHR && f->node->dev == LXDEV_OKWIN) { extern int lx_okwin_ready(void*); if (lx_okwin_ready(f->priv)) r |= POLLIN; r |= POLLOUT; break; }
        r |= POLLIN | POLLOUT; break;
    default: r |= POLLIN | POLLOUT; break;
    }
    return r & (want | POLLERR | POLLHUP | POLLNVAL);
}

struct lx_pollfd { int32_t fd; int16_t events, revents; };

static int poll_wake(struct lx_task *t) {
    if (t->wake_ms && lx_now_ms() >= (int64_t)t->wake_ms) return 1;
    uint64_t u = t->regs.rdi; uint32_t n = (uint32_t)t->regs.rsi;
    for (uint32_t i = 0; i < n && i < 256; i++) {
        struct lx_pollfd p;
        if (lx_copy_in(t, &p, u + i * 8, 8)) return 1;
        if (p.fd >= 0 && fd_events(t, fd_get(t, p.fd), p.events)) return 1;
    }
    return 0;
}

static int64_t do_poll(struct lx_task *t, uint64_t ufds, uint64_t n, int64_t timeout_ms) {
    if (n > 256) return -LX_EINVAL;
    int ready = 0;
    for (uint64_t i = 0; i < n; i++) {
        struct lx_pollfd p;
        if (lx_copy_in(t, &p, ufds + i * 8, 8)) return -LX_EFAULT;
        p.revents = 0;
        if (p.fd >= 0) p.revents = (int16_t)fd_events(t, fd_get(t, p.fd), p.events);
        if (p.revents) ready++;
        if (lx_copy_out(t, ufds + i * 8 + 6, &p.revents, 2)) return -LX_EFAULT;
    }
    if (ready || timeout_ms == 0) { t->wake_ms = 0; return ready; }
    if (timeout_ms > 0) {
        if (!t->wake_ms) t->wake_ms = (uint64_t)(lx_now_ms() + timeout_ms);
        else if (lx_now_ms() >= (int64_t)t->wake_ms) { t->wake_ms = 0; return 0; }
    }
    lx_block(t, poll_wake, 0);
    return 0;
}

// ---- the dispatcher ---------------------------------------------------------------------------------------------

int64_t lx_syscall_dispatch(struct lx_task *t) {
    struct lx_regs *R = &t->regs;
    uint64_t nr = R->rax;
    uint64_t a0 = R->rdi, a1 = R->rsi, a2 = R->rdx, a3 = R->r10, a4 = R->r8, a5 = R->r9;
    struct lx_file *f;
    int err;
    switch (nr) {
    case 0:   // read
        f = fd_get(t, (int64_t)a0); if (!f) return -LX_EBADF;
        return file_read(t, f, a1, a2, 0, 0);
    case 1:   // write
        f = fd_get(t, (int64_t)a0); if (!f) return -LX_EBADF;
        return file_write(t, f, a1, a2, 0, 0);
    case 2:   // open
        return do_openat(t, AT_FDCWD, a0, (uint32_t)a1, (uint32_t)a2);
    case 257: // openat
        return do_openat(t, (int32_t)a0, a1, (uint32_t)a2, (uint32_t)a3);
    case 85:  // creat
        return do_openat(t, AT_FDCWD, a0, LX_O_CREAT | LX_O_WRONLY | LX_O_TRUNC, (uint32_t)a1);
    case 3:   // close
        f = fd_get(t, (int64_t)a0); if (!f) return -LX_EBADF;
        lx_file_put(f); t->files->fds[a0] = 0; t->files->fdflags[a0] = 0;
        return 0;
    case 4: case 6: { // stat, lstat
        struct lx_node *n = path_node(t, AT_FDCWD, a0, nr == 4, &err);
        if (!n) return err;
        return do_stat_node(t, n, a1);
    }
    case 5:   // fstat
        f = fd_get(t, (int64_t)a0); if (!f) return -LX_EBADF;
        return do_stat_file(t, f, a1);
    case 262: { // newfstatat
        uint32_t fl = (uint32_t)a3;
        if ((fl & 0x1000) /* AT_EMPTY_PATH */) {
            char c;
            if (lx_copy_in(t, &c, a1, 1) == 0 && c == 0) {
                f = fd_get(t, (int32_t)a0); if (!f) return -LX_EBADF;
                return do_stat_file(t, f, a2);
            }
        }
        struct lx_node *n = path_node(t, (int32_t)a0, a1, !(fl & 0x100), &err);
        if (!n) return err;
        return do_stat_node(t, n, a2);
    }
    case 332: { // statx (enough for the C library's fstat fallback)
        char path[16];
        struct lx_node *n = 0;
        if (lx_copy_str(t, path, a1, sizeof path) == 0 && (a2 & 0x1000)) {
            f = fd_get(t, (int32_t)a0); if (!f) return -LX_EBADF;
            n = f->node;
        } else {
            n = path_node(t, (int32_t)a0, a1, !(a2 & 0x100), &err);
            if (!n) return err;
        }
        uint8_t sx[256];
        memset(sx, 0, sizeof sx);
        uint32_t mask = 0x7FF;                                   // BASIC_STATS
        memcpy(sx, &mask, 4);
        uint32_t blk = 4096; memcpy(sx + 4, &blk, 4);
        uint32_t nlink = n ? (n->nlink ? n->nlink : 1) : 1; memcpy(sx + 16, &nlink, 4);
        uint16_t mode = n ? (uint16_t)node_mode(n) : 0020620; memcpy(sx + 28, &mode, 2);
        uint64_t ino = n ? n->ino : 0, size = n ? n->size : 0; memcpy(sx + 32, &ino, 8); memcpy(sx + 40, &size, 8);
        uint64_t blocks = (size + 511) / 512; memcpy(sx + 48, &blocks, 8);
        int64_t mt = n ? n->mtime : 0; memcpy(sx + 64, &mt, 8); memcpy(sx + 80, &mt, 8); memcpy(sx + 96, &mt, 8);
        return lx_copy_out(t, a4, sx, sizeof sx);
    }
    case 8: { // lseek
        f = fd_get(t, (int64_t)a0); if (!f) return -LX_EBADF;
        if (f->kind != LXF_NODE) return -LX_ESPIPE;
        int64_t base = a2 == 0 ? 0 : a2 == 1 ? (int64_t)f->off : (int64_t)(f->node ? f->node->size : 0);
        if (a2 > 2) return -LX_EINVAL;
        int64_t np = base + (int64_t)a1;
        if (np < 0) return -LX_EINVAL;
        f->off = (uint64_t)np;
        return np;
    }
    case 17: case 18: { // pread64, pwrite64
        f = fd_get(t, (int64_t)a0); if (!f) return -LX_EBADF;
        if (f->kind != LXF_NODE) return -LX_ESPIPE;
        return nr == 17 ? file_read(t, f, a1, a2, 1, a3) : file_write(t, f, a1, a2, 1, a3);
    }
    case 19: case 20: { // readv, writev
        f = fd_get(t, (int64_t)a0); if (!f) return -LX_EBADF;
        if (a2 > 1024) return -LX_EINVAL;
        int64_t total = 0;
        for (uint64_t i = 0; i < a2; i++) {
            uint64_t iov[2];
            if (lx_copy_in(t, iov, a1 + i * 16, 16)) return total ? total : -LX_EFAULT;
            if (!iov[1]) continue;
            int64_t r = nr == 19 ? file_read(t, f, iov[0], iov[1], 0, 0) : file_write(t, f, iov[0], iov[1], 0, 0);
            if (t->state == LXT_BLOCK) { if (total) { t->state = LXT_RUN; return total; } return 0; }
            if (r < 0) return total ? total : r;
            total += r;
            if ((uint64_t)r < iov[1]) break;
        }
        return total;
    }
    case 9: // mmap
        return do_mmap(t, a0, a1, (uint32_t)a2, (uint32_t)a3, (int64_t)(int32_t)a4, a5);
    case 10: // mprotect
        if (a0 & 0xFFF) return -LX_EINVAL;
        return lx_protect_region(t, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2);
    case 11: // munmap
        if (a0 & 0xFFF || !a1) return -LX_EINVAL;
        return lx_unmap_region(t, (uint32_t)a0, (uint32_t)a1);
    case 12: // brk
        return do_brk(t, a0);
    case 25: { // mremap (move or resize)
        struct lx_vma *v = lx_vma_find(t, (uint32_t)a0);
        if (!v || v->start != a0) return -LX_EINVAL;
        uint32_t olen = (uint32_t)((a1 + 0xFFF) & ~0xFFFull), nlen = (uint32_t)((a2 + 0xFFF) & ~0xFFFull);
        if (nlen == olen) return (int64_t)a0;
        if (nlen < olen) { lx_unmap_region(t, (uint32_t)a0 + nlen, olen - nlen); return (int64_t)a0; }
        // grow in place if the space after is free, else move
        int free = 1;
        for (int i = 0; i < t->mm->nvma; i++) if ((uint32_t)a0 + olen < t->mm->vma[i].end && (uint32_t)a0 + nlen > t->mm->vma[i].start && t->mm->vma[i].start != a0) free = 0;
        if (free && (uint32_t)a0 + nlen <= LX_USER_TOP) {
            if (lx_map_region(t, (uint32_t)a0 + olen, nlen - olen, v->prot, v->flags, 0, 0)) return -LX_ENOMEM;
            return (int64_t)a0;
        }
        if (!(a3 & 1)) return -LX_ENOMEM;                         // MREMAP_MAYMOVE
        uint32_t ns = lx_find_free(t, nlen);
        if (!ns) return -LX_ENOMEM;
        uint32_t prot = v->prot, fl = v->flags;
        if (lx_map_region(t, ns, nlen, prot, fl, 0, 0)) return -LX_ENOMEM;
        for (uint32_t o = 0; o < olen; o += 0x1000) {                 // move the touched pages
            uint32_t ph = aspace_unmap(t->mm->cr3, (uint32_t)a0 + o);
            if (ph) { if (aspace_map(t->mm->cr3, ns + o, ph, (prot & LX_PROT_WRITE) != 0)) pmm_free_page(ph); }
        }
        lx_unmap_region(t, (uint32_t)a0, olen);
        return ns;
    }
    case 28: return 0;                                              // madvise
    case 13: { // rt_sigaction(sig, act, oldact, sigsetsize)
        int sig = (int)a0;
        if (sig <= 0 || sig >= LX_NSIG || sig == 9 || sig == 19) return -LX_EINVAL;
        if (a2) {
            struct lx_sigaction o = t->sh->act[sig];
            if (lx_copy_out(t, a2, &o, 32)) return -LX_EFAULT;
        }
        if (a1) {
            struct lx_sigaction n;
            if (lx_copy_in(t, &n, a1, 32)) return -LX_EFAULT;
            t->sh->act[sig] = n;
            if (n.handler == 1 || (n.handler == 0 && (sig == 17 || sig == 18 || sig == 23 || sig == 28))) t->sig_pending &= ~(1ull << (sig - 1));
        }
        return 0;
    }
    case 14: { // rt_sigprocmask(how, set, oldset, size)
        uint64_t old = t->sig_blocked;
        if (a1) {
            uint64_t s;
            if (lx_copy_in(t, &s, a1, 8)) return -LX_EFAULT;
            s &= ~((1ull << 8) | (1ull << 18));                         // SIGKILL and SIGSTOP cannot be blocked
            if (a0 == 0) t->sig_blocked |= s; else if (a0 == 1) t->sig_blocked &= ~s; else if (a0 == 2) t->sig_blocked = s; else return -LX_EINVAL;
        }
        if (a2 && lx_copy_out(t, a2, &old, 8)) return -LX_EFAULT;
        return 0;
    }
    case 127: { uint64_t p = t->sig_pending & t->sig_blocked; return lx_copy_out(t, a0, &p, 8); }   // rt_sigpending
    case 131: { // sigaltstack(ss, old)
        if (a1) { uint64_t o[3] = { t->sigaltstack_sp, 0, t->sigaltstack_size }; if (!t->sigaltstack_size) o[1] = 2; if (lx_copy_out(t, a1, o, 24)) return -LX_EFAULT; }
        if (a0) { uint64_t n[3]; if (lx_copy_in(t, n, a0, 24)) return -LX_EFAULT; t->sigaltstack_sp = n[0]; t->sigaltstack_size = n[2]; if ((n[1] & 0xFFFFFFFFull) & 2) t->sigaltstack_size = 0; }
        return 0;
    }
    case 15: return lx_sys_rt_sigreturn(t);                          // rt_sigreturn
    case 34: { // pause
        if (t->sig_pending & ~t->sig_blocked) return -LX_EINTR;
        lx_block(t, sig_wait_ready, 0);
        t->block_complete = 1;
        return -LX_EINTR;
    }
    case 130: { // rt_sigsuspend(mask, size)
        uint64_t m;
        if (lx_copy_in(t, &m, a0, 8)) return -LX_EFAULT;
        t->saved_mask = t->sig_blocked;
        t->sig_blocked = m & ~((1ull << 8) | (1ull << 18));
        lx_block(t, sig_wait_ready, 0);
        t->block_complete = 1;
        return -LX_EINTR;
    }
    case 36: return lx_sys_getitimer(t, a0, a1);
    case 37: return lx_sys_alarm(t, a0);
    case 38: return lx_sys_setitimer(t, a0, a1, a2);
    case 16: { // ioctl
        f = fd_get(t, (int64_t)a0); if (!f) return -LX_EBADF;
        if (f->kind == LXF_TTY || (f->node && f->node->type == LXN_CHR && (f->node->dev == LXDEV_TTY || f->node->dev == LXDEV_CONSOLE)))
            return lx_tty_ioctl(t, a1, a2);
        if (f->node && f->node->type == LXN_CHR && f->node->dev == LXDEV_FB0) {
            extern int lx_fb_ioctl(struct lx_task*, uint64_t, uint64_t);
            return lx_fb_ioctl(t, a1, a2);
        }
        if (f->node && f->node->type == LXN_CHR && f->node->dev == LXDEV_OKWIN) {
            extern int64_t lx_okwin_ioctl(struct lx_task*, void*, uint64_t, uint64_t);
            return lx_okwin_ioctl(t, f->priv, a1, a2);
        }
        return -LX_ENOTTY;
    }
    case 7:   // poll
        return do_poll(t, a0, a1, (int64_t)(int32_t)a2);
    case 21: case 269: { // access, faccessat
        struct lx_node *n = path_node(t, nr == 21 ? AT_FDCWD : (int32_t)a0, nr == 21 ? a0 : a1, 1, &err);
        return n ? 0 : err;
    }
    case 22: case 293: { // pipe, pipe2
        struct lx_file *r, *w;
        int e = pipe_new(&r, &w);
        if (e) return e;
        uint32_t fl = nr == 293 ? (uint32_t)a1 : 0;
        if (fl & LX_O_NONBLOCK) { r->flags |= LX_O_NONBLOCK; w->flags |= LX_O_NONBLOCK; }
        int fr = lx_fd_alloc(t, r, 0, (fl & LX_O_CLOEXEC) != 0);
        int fw = fr >= 0 ? lx_fd_alloc(t, w, 0, (fl & LX_O_CLOEXEC) != 0) : -LX_EMFILE;
        if (fr < 0 || fw < 0) { if (fr >= 0) t->files->fds[fr] = 0; lx_file_put(r); lx_file_put(w); return -LX_EMFILE; }
        int32_t fds[2] = { fr, fw };
        if (lx_copy_out(t, a0, fds, 8)) return -LX_EFAULT;
        return 0;
    }
    case 24: return 0;                                              // sched_yield
    case 32: { // dup
        f = fd_get(t, (int64_t)a0); if (!f) return -LX_EBADF;
        f->refs++;
        int nfd = lx_fd_alloc(t, f, 0, 0);
        if (nfd < 0) lx_file_put(f);
        return nfd;
    }
    case 33: case 292: { // dup2, dup3
        f = fd_get(t, (int64_t)a0); if (!f) return -LX_EBADF;
        if (a1 >= LX_MAX_FDS) return -LX_EBADF;
        if (a0 == a1) return nr == 292 ? -LX_EINVAL : (int64_t)a1;
        if (t->files->fds[a1]) lx_file_put(t->files->fds[a1]);
        f->refs++;
        t->files->fds[a1] = f; t->files->fdflags[a1] = (nr == 292 && (a2 & LX_O_CLOEXEC)) ? 1 : 0;
        return (int64_t)a1;
    }
    case 72: { // fcntl
        f = fd_get(t, (int64_t)a0); if (!f) return -LX_EBADF;
        switch (a1) {
        case 0: case 1030: { f->refs++; int nfd = lx_fd_alloc(t, f, (int)a2, a1 == 1030); if (nfd < 0) lx_file_put(f); return nfd; }  // DUPFD, DUPFD_CLOEXEC
        case 1: return t->files->fdflags[a0] & 1;                                  // GETFD
        case 2: t->files->fdflags[a0] = (uint8_t)(a2 & 1); return 0;               // SETFD
        case 3: return f->flags;                                            // GETFL
        case 4: f->flags = (f->flags & LX_O_ACCMODE) | (int)(a2 & ~(uint64_t)LX_O_ACCMODE); return 0;   // SETFL
        default: return 0;
        }
    }
    case 35: return do_sleep(t, a0);                                // nanosleep
    case 230: { // clock_nanosleep(clk, flags, req, rem)
        if (a1 & 1) {                                               // TIMER_ABSTIME
            struct lx_timespec ts, now;
            if (lx_copy_in(t, &ts, a2, sizeof ts)) return -LX_EFAULT;
            clock_get((int)a0, &now);
            int64_t d = (ts.sec - now.sec) * 1000000000 + (ts.nsec - now.nsec);
            if (d <= 0) return 0;
            struct lx_timespec rel = { d / 1000000000, d % 1000000000 };
            if (!t->wake_ms) { t->wake_ms = (uint64_t)(lx_now_ms() + (d + 999999) / 1000000); }
            (void)rel;
            if (lx_now_ms() >= (int64_t)t->wake_ms) { t->wake_ms = 0; return 0; }
            lx_block(t, sleep_done, 0);
            return 0;
        }
        return do_sleep(t, a2);
    }
    case 39: return t->tgid;                                        // getpid
    case 186: return t->pid;                                        // gettid
    case 110: return t->ppid;                                       // getppid
    case 102: case 104: case 107: case 108: return 0;               // uid gid euid egid
    case 111: return t->pid;                                        // getpgrp
    case 121: return t->pid;                                        // getpgid
    case 112: t->term_win = -1; return t->pid;                         // setsid: a daemon leaves its terminal (the shell prompt comes back, output goes to the serial log)
    case 109: case 105: case 106: return 0;                         // setpgid setuid setgid
    case 218: t->clear_child_tid = a0; return t->pid;               // set_tid_address
    case 273: return 0;                                             // set_robust_list
    case 334: case 435: return -LX_ENOSYS;                          // rseq, clone3
    case 204: { // sched_getaffinity: one CPU
        uint64_t m = 1;
        uint64_t sz = a1 < 8 ? a1 : 8;
        if (lx_copy_out(t, a2, &m, sz)) return -LX_EFAULT;
        return (int64_t)sz;
    }
    case 95: { uint32_t o = t->umask; t->umask = (uint32_t)a0 & 0777; return o; }   // umask
    case 63: { // uname
        char u[6 * 65];
        memset(u, 0, sizeof u);
        strncpy(u, "Linux", 64); strncpy(u + 65, "kanarchy", 64); strncpy(u + 130, "6.1.0-kanarchy", 64);
        strncpy(u + 195, "#1 SMP KAnarchy", 64); strncpy(u + 260, "x86_64", 64); strncpy(u + 325, "(none)", 64);
        return lx_copy_out(t, a0, u, sizeof u);
    }
    case 79: { // getcwd
        char p[LX_PATH_MAX];
        lxfs_path_of(t->cwd, p, sizeof p);
        uint64_t l = strlen(p) + 1;
        if (l > a1) return -LX_ERANGE;
        if (lx_copy_out(t, a0, p, l)) return -LX_EFAULT;
        return (int64_t)l;
    }
    case 80: { // chdir
        struct lx_node *n = path_node(t, AT_FDCWD, a0, 1, &err);
        if (!n) return err;
        if (n->type != LXN_DIR) return -LX_ENOTDIR;
        t->cwd = n;
        return 0;
    }
    case 81: { // fchdir
        f = fd_get(t, (int64_t)a0); if (!f || !f->node) return -LX_EBADF;
        if (f->node->type != LXN_DIR) return -LX_ENOTDIR;
        t->cwd = f->node;
        return 0;
    }
    case 83: case 258: { // mkdir, mkdirat
        struct lx_node *dir; char name[LX_NAME_MAX + 1];
        int r = path_parent(t, nr == 83 ? AT_FDCWD : (int32_t)a0, nr == 83 ? a0 : a1, &dir, name);
        if (r) return r;
        if (!lxfs_create(dir, name, LXN_DIR, (int)((nr == 83 ? a1 : a2) & 07777 & ~t->umask), &err)) return err;
        return 0;
    }
    case 84: case 87: case 263: { // rmdir, unlink, unlinkat
        struct lx_node *dir; char name[LX_NAME_MAX + 1];
        int want_dir = nr == 84 || (nr == 263 && (a2 & 0x200));
        int r = path_parent(t, nr == 263 ? (int32_t)a0 : AT_FDCWD, nr == 263 ? a1 : a0, &dir, name);
        if (r) return r;
        return lxfs_unlink(dir, name, want_dir);
    }
    case 82: case 264: { // rename, renameat
        struct lx_node *od, *nd; char on[LX_NAME_MAX + 1], nn[LX_NAME_MAX + 1];
        int r = path_parent(t, nr == 264 ? (int32_t)a0 : AT_FDCWD, nr == 264 ? a1 : a0, &od, on);
        if (r) return r;
        r = path_parent(t, nr == 264 ? (int32_t)a2 : AT_FDCWD, nr == 264 ? a3 : a1, &nd, nn);
        if (r) return r;
        return lxfs_rename(od, on, nd, nn);
    }
    case 88: case 266: { // symlink, symlinkat
        char target[LX_PATH_MAX];
        int l = lx_copy_str(t, target, a0, sizeof target);
        if (l < 0) return l;
        struct lx_node *dir; char name[LX_NAME_MAX + 1];
        int r = path_parent(t, nr == 266 ? (int32_t)a1 : AT_FDCWD, nr == 266 ? a2 : a1, &dir, name);
        if (r) return r;
        return lxfs_symlink(dir, name, target);
    }
    case 89: case 267: { // readlink, readlinkat
        uint64_t up = nr == 89 ? a0 : a1, ub = nr == 89 ? a1 : a2, ul = nr == 89 ? a2 : a3;
        char path[LX_PATH_MAX];
        int l = lx_copy_str(t, path, up, sizeof path);
        if (l < 0) return l;
        const char *tgt = 0;
        if (strcmp(path, "/proc/self/exe") == 0) tgt = t->exe;
        else {
            struct lx_node *n = path_node(t, nr == 89 ? AT_FDCWD : (int32_t)a0, up, 0, &err);
            if (!n) return err;
            if (n->type != LXN_LNK) return -LX_EINVAL;
            tgt = n->target;
        }
        uint64_t tl = strlen(tgt);
        if (tl > ul) tl = ul;
        if (lx_copy_out(t, ub, tgt, tl)) return -LX_EFAULT;
        return (int64_t)tl;
    }
    case 90: case 91: case 92: case 93: case 94: return 0;        // chmod fchmod chown fchown lchown
    case 268: case 280: return 0;                                  // fchmodat utimensat
    case 74: case 75: case 76: return 0;                           // fsync fdatasync truncate-less
    case 77: { // ftruncate
        f = fd_get(t, (int64_t)a0); if (!f || !f->node) return -LX_EBADF;
        return lxfs_truncate(f->node, a1);
    }
    case 217: case 78: { // getdents64, getdents
        f = fd_get(t, (int64_t)a0); if (!f) return -LX_EBADF;
        return do_getdents64(t, f, a1, a2);
    }
    case 96: { // gettimeofday
        struct lx_timespec ts;
        clock_get(0, &ts);
        int64_t tv[2] = { ts.sec, ts.nsec / 1000 };
        if (a0 && lx_copy_out(t, a0, tv, 16)) return -LX_EFAULT;
        return 0;
    }
    case 201: { // time
        int64_t s = kclock_epoch_ms() / 1000;
        if (a0 && lx_copy_out(t, a0, &s, 8)) return -LX_EFAULT;
        return s;
    }
    case 228: { // clock_gettime
        struct lx_timespec ts;
        clock_get((int)a0, &ts);
        return lx_copy_out(t, a1, &ts, 16);
    }
    case 229: { struct lx_timespec ts = { 0, 1000000 }; return a1 ? lx_copy_out(t, a1, &ts, 16) : 0; }   // clock_getres
    case 97: case 302: { // getrlimit, prlimit64
        uint64_t resource = nr == 97 ? a0 : a1, ures = nr == 97 ? a1 : a3;
        uint64_t lim[2] = { ~0ull, ~0ull };
        if (resource == 3) { lim[0] = LX_STACK_SIZE; }                 // RLIMIT_STACK
        else if (resource == 7) { lim[0] = lim[1] = LX_MAX_FDS; }      // RLIMIT_NOFILE
        if (ures && lx_copy_out(t, ures, lim, 16)) return -LX_EFAULT;
        return 0;
    }
    case 98: return 0;                                              // getrusage (zeros)
    case 99: { // sysinfo
        uint64_t si[14];
        memset(si, 0, sizeof si);
        si[0] = (uint64_t)(lx_now_ms() / 1000);
        si[2] = (uint64_t)pmm_get_total_pages() * 4096; si[3] = (uint64_t)pmm_get_free_pages() * 4096;
        uint32_t procs = 1; memcpy((uint8_t*)si + 80, &procs, 2);
        si[11] = 1;                                                 // mem_unit placeholder below
        uint8_t raw[112]; memset(raw, 0, sizeof raw);
        memcpy(raw, &si[0], 8);
        memcpy(raw + 32, &si[2], 8); memcpy(raw + 40, &si[3], 8);
        uint16_t pr = 1; memcpy(raw + 80, &pr, 2);
        uint32_t unit = 1; memcpy(raw + 104, &unit, 4);
        return lx_copy_out(t, a0, raw, sizeof raw);
    }
    case 158: { // arch_prctl
        if (a0 == 0x1002) { t->fs_base = a1; __asm__ volatile("wrmsr" :: "c"(0xC0000100), "a"((uint32_t)a1), "d"((uint32_t)(a1 >> 32))); return 0; }
        if (a0 == 0x1003) return lx_copy_out(t, a1, &t->fs_base, 8);
        if (a0 == 0x1005) { t->win_exc_entry = a1; return 0; }                       // the Windows exception entry
        if (a0 == 0x1001) { t->gs_base = a1; __asm__ volatile("wrmsr" :: "c"(0xC0000101), "a"((uint32_t)a1), "d"((uint32_t)(a1 >> 32))); return 0; }
        if (a0 == 0x1004) return lx_copy_out(t, a1, &t->gs_base, 8);
        return -LX_EINVAL;
    }
    case 202: { // futex
        uint32_t op = (uint32_t)a1 & 0x7F;
        if (op == 0 || op == 9) {                                   // WAIT, WAIT_BITSET
            if (t->futex_state == 2) { t->futex_state = 0; t->wake_ms = 0; return 0; }
            if (t->futex_state == 3) { t->futex_state = 0; t->wake_ms = 0; return -LX_ETIMEDOUT; }
            uint32_t cur;
            if (lx_copy_in(t, &cur, a0, 4)) return -LX_EFAULT;
            if (cur != (uint32_t)a2) return -LX_EAGAIN;
            if (a3 && !t->wake_ms) {
                struct lx_timespec ts;
                if (lx_copy_in(t, &ts, a3, 16)) return -LX_EFAULT;
                int64_t ms = ts.sec * 1000 + (ts.nsec + 999999) / 1000000;
                if (op == 9) { struct lx_timespec now; clock_get((a1 & 0x100) ? 0 : 1, &now); ms = (ts.sec - now.sec) * 1000 + (ts.nsec - now.nsec) / 1000000; if (ms < 0) ms = 0; }
                t->wake_ms = (uint64_t)(lx_now_ms() + ms + 1);
            }
            t->futex_addr = a0;
            t->futex_state = 1;
            lx_block(t, futex_ready, 0);
            return 0;
        }
        if (op == 1 || op == 10) return lx_futex_wake(t->mm, a0, (int)a2);                  // WAKE, WAKE_BITSET
        if (op == 3 || op == 4) {                                                            // REQUEUE, CMP_REQUEUE
            if (op == 4) { uint32_t cur; if (lx_copy_in(t, &cur, a0, 4)) return -LX_EFAULT; if (cur != (uint32_t)a5) return -LX_EAGAIN; }
            int woken = lx_futex_wake(t->mm, a0, (int)a2);
            lx_futex_requeue(t->mm, a0, a4, (int)a3);
            return woken;
        }
        if (op == 5) {                                                                       // WAKE_OP: *uaddr2 op= arg; wake uaddr1 (v1), uaddr2 (v2) if cmp
            uint32_t val; uint32_t enc = (uint32_t)a5;
            if (lx_copy_in(t, &val, a4, 4)) return -LX_EFAULT;
            uint32_t oldv = val; int opn = (enc >> 28) & 7, cmp = (enc >> 24) & 15; int32_t oparg = (int32_t)(enc << 8) >> 20, cmparg = (int32_t)(enc << 20) >> 20;
            if ((enc >> 28) & 8) oparg = 1 << oparg;
            switch (opn) { case 0: val = (uint32_t)oparg; break; case 1: val += (uint32_t)oparg; break; case 2: val |= (uint32_t)oparg; break; case 3: val &= ~(uint32_t)oparg; break; case 4: val ^= (uint32_t)oparg; break; }
            if (lx_copy_out(t, a4, &val, 4)) return -LX_EFAULT;
            int woken = lx_futex_wake(t->mm, a0, (int)a2), cond = 0;
            switch (cmp) { case 0: cond = (int32_t)oldv == cmparg; break; case 1: cond = (int32_t)oldv != cmparg; break; case 2: cond = (int32_t)oldv < cmparg; break; case 3: cond = (int32_t)oldv <= cmparg; break; case 4: cond = (int32_t)oldv > cmparg; break; case 5: cond = (int32_t)oldv >= cmparg; break; }
            if (cond) woken += lx_futex_wake(t->mm, a4, (int)a3);
            return woken;
        }
        return 0;
    }
    case 318: { // getrandom
        uint8_t buf[256];
        uint64_t left = a1, p = a0;
        while (left) {
            uint32_t k = left > sizeof buf ? sizeof buf : (uint32_t)left;
            random_fill(buf, k);
            if (lx_copy_out(t, p, buf, k)) return -LX_EFAULT;
            p += k; left -= k;
        }
        return (int64_t)a1;
    }
    case 57: case 58: { // fork, vfork
        uint64_t fl = nr == 58 ? (0x100 | 0x4000 | 17) : 17;
        struct lx_task *c = lx_clone(t, fl, 0, 0, 0, 0);
        if (!c) return -LX_ENOMEM;
        if (nr == 58) { t->block_arg = (uint64_t)c->pid; lx_block(t, vfork_ready, (uint64_t)c->pid); t->block_complete = 1; }
        return c->pid;
    }
    case 56: { // clone(flags, stack, ptid, ctid, tls)
        struct lx_task *c = lx_clone(t, a0, a1, a2, a3, a4);
        if (!c) return -LX_ENOSYS;
        if (a0 & 0x4000) { lx_block(t, vfork_ready, (uint64_t)c->pid); t->block_complete = 1; }
        return c->pid;
    }
    case 59: return do_execve(t, a0, a1, a2);
    case 60: // exit: this thread
        lx_thread_exit(t, ((int)a0 & 0xFF) << 8);
        return 0;
    case 231: // exit_group
        lx_task_exit(t, ((int)a0 & 0xFF) << 8);
        return 0;
    case 61: return do_wait4(t, (int64_t)a0, a1, (uint32_t)a2);
    case 62: { // kill
        int64_t pid = (int64_t)a0;
        if (pid == 0 || pid == -1) pid = t->tgid;
        if (pid < 0) pid = -pid;
        struct lx_task *v = 0;
        if (lx_task_by_pid((int)pid, &v)) {
            if (a1) lx_signal_group(v->tgid, (int)a1);
            return 0;
        }
        return -LX_ESRCH;
    }
    case 200: case 234: { // tkill, tgkill
        struct lx_task *v = 0;
        uint64_t pid = nr == 200 ? a0 : a1, sig = nr == 200 ? a1 : a2;
        if (lx_task_by_pid((int)pid, &v)) { if (sig) lx_signal(v, (int)sig); return 0; }
        return -LX_ESRCH;
    }
    case 137: case 138: { // statfs, fstatfs: one big memory file system
        uint64_t sf[15];
        memset(sf, 0, sizeof sf);
        sf[0] = 0x01021994;                                         // TMPFS_MAGIC
        sf[1] = 4096; sf[2] = pmm_get_total_pages(); sf[3] = sf[4] = pmm_get_free_pages();
        sf[5] = 1 << 20; sf[6] = 1 << 19; sf[9] = 255; sf[10] = 4096;
        return lx_copy_out(t, a1, sf, sizeof sf);
    }
    default:
        return lx_syscall_more(t, nr, a0, a1, a2, a3, a4, a5);
    }
}

int lx_fd_events(struct lx_task *t, struct lx_file *f, int want) { return fd_events(t, f, want); }
