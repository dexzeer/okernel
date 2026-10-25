// epoll, eventfd and timerfd of the Linux subsystem.
#include "lx.h"
#include "memory.h"
#include "serial.h"
#include "string.h"

extern int lx_fd_events(struct lx_task *t, struct lx_file *f, int want);

#define EV_EPOLL 1
#define EV_EVENTFD 2
#define EV_TIMERFD 3
#define EV_INOTIFY 4

#define EP_MAX 128

struct lx_event {
    int kind;
    struct { int fd; struct lx_file *f; uint32_t events; uint64_t data; int active; } ep[EP_MAX];
    uint64_t counter; int semaphore;                                  // eventfd
    uint64_t next_ms, interval_ms, expirations;                       // timerfd
};

void lx_event_free(struct lx_event *e) {
    if (e->kind == EV_EPOLL) for (int i = 0; i < EP_MAX; i++) if (e->ep[i].active) lx_file_put(e->ep[i].f);
    kfree(e);
}

static struct lx_file *file_of(struct lx_task *t, int64_t fd) {
    return (fd >= 0 && fd < LX_MAX_FDS) ? t->files->fds[fd] : 0;
}

static int new_event_fd(struct lx_task *t, int kind, int flags) {
    struct lx_event *e = (struct lx_event*)kcalloc(1, sizeof *e);
    if (!e) return -LX_ENOMEM;
    e->kind = kind;
    struct lx_file *f = lx_file_new(LXF_EVENT, LX_O_RDWR | ((flags & 04000) ? LX_O_NONBLOCK : 0), 0);
    if (!f) { kfree(e); return -LX_ENOMEM; }
    f->ev = e;
    int fd = lx_fd_alloc(t, f, 0, (flags & 02000000) != 0);
    if (fd < 0) lx_file_put(f);
    return fd;
}

int64_t lx_sys_epoll_create(struct lx_task *t, int flags) { return new_event_fd(t, EV_EPOLL, flags); }
int64_t lx_sys_eventfd(struct lx_task *t, uint64_t initval, int flags) {
    int fd = new_event_fd(t, EV_EVENTFD, flags);
    if (fd >= 0) { struct lx_event *e = t->files->fds[fd]->ev; e->counter = initval; e->semaphore = (flags & 1) != 0; }
    return fd;
}
int64_t lx_sys_timerfd_create(struct lx_task *t, int clock, int flags) { (void)clock; return new_event_fd(t, EV_TIMERFD, flags); }
int64_t lx_sys_inotify_init(struct lx_task *t, int flags) { return new_event_fd(t, EV_INOTIFY, flags); }

struct ep_event { uint32_t events; uint64_t data; } __attribute__((packed));

int64_t lx_sys_epoll_ctl(struct lx_task *t, int epfd, int op, int fd, uint64_t uev) {
    struct lx_file *ef = file_of(t, epfd), *tf = file_of(t, fd);
    if (!ef || ef->kind != LXF_EVENT || ef->ev->kind != EV_EPOLL || !tf) return -LX_EBADF;
    struct lx_event *e = ef->ev;
    struct ep_event ev = { 0, 0 };
    if (op != 2 && lx_copy_in(t, &ev, uev, sizeof ev)) return -LX_EFAULT;
    int slot = -1, free = -1;
    for (int i = 0; i < EP_MAX; i++) {
        if (e->ep[i].active && e->ep[i].fd == fd && e->ep[i].f == tf) slot = i;
        if (!e->ep[i].active && free < 0) free = i;
    }
    if (op == 1) {                                                    // ADD
        if (slot >= 0) return -LX_EEXIST;
        if (free < 0) return -LX_ENOMEM;
        e->ep[free].fd = fd; e->ep[free].f = tf; tf->refs++;
        e->ep[free].events = ev.events; e->ep[free].data = ev.data; e->ep[free].active = 1;
        return 0;
    }
    if (slot < 0) return -LX_ENOENT;
    if (op == 2) { lx_file_put(e->ep[slot].f); e->ep[slot].active = 0; return 0; }          // DEL
    if (op == 3) { e->ep[slot].events = ev.events; e->ep[slot].data = ev.data; return 0; }  // MOD
    return -LX_EINVAL;
}

static int collect(struct lx_task *t, struct lx_event *e, struct ep_event *out, int max) {
    int n = 0;
    for (int i = 0; i < EP_MAX && n < max; i++) {
        if (!e->ep[i].active) continue;
        int r = lx_fd_events(t, e->ep[i].f, (int)(e->ep[i].events & 0x1F));
        r &= (int)(e->ep[i].events & 0x1F) | 0x18;                    // always report ERR and HUP
        if (!r) continue;
        out[n].events = (uint32_t)r;
        out[n].data = e->ep[i].data;
        n++;
        if (e->ep[i].events & (1u << 30)) e->ep[i].events &= ~0x1Fu;  // ONESHOT: disarmed until MOD
    }
    return n;
}

static int epoll_ready(struct lx_task *t) {
    if (t->wake_ms && lx_now_ms() >= (int64_t)t->wake_ms) return 1;
    struct lx_file *ef = file_of(t, (int64_t)t->regs.rdi);
    if (!ef || ef->kind != LXF_EVENT) return 1;
    struct ep_event probe[1];
    for (int i = 0; i < EP_MAX; i++) {
        struct lx_event *e = ef->ev;
        if (!e->ep[i].active) continue;
        int r = lx_fd_events(t, e->ep[i].f, (int)(e->ep[i].events & 0x1F)) & ((int)(e->ep[i].events & 0x1F) | 0x18);
        (void)probe;
        if (r) return 1;
    }
    return 0;
}

int64_t lx_sys_epoll_wait(struct lx_task *t, int epfd, uint64_t uevents, int maxevents, int64_t timeout_ms) {
    struct lx_file *ef = file_of(t, epfd);
    if (!ef || ef->kind != LXF_EVENT || ef->ev->kind != EV_EPOLL) return -LX_EBADF;
    if (maxevents <= 0 || maxevents > 64) return -LX_EINVAL;
    struct ep_event out[64];
    int n = collect(t, ef->ev, out, maxevents);
    if (n > 0 || timeout_ms == 0) {
        t->wake_ms = 0;
        if (n > 0 && lx_copy_out(t, uevents, out, (uint64_t)n * sizeof(struct ep_event))) return -LX_EFAULT;
        return n;
    }
    if (timeout_ms > 0) {
        if (!t->wake_ms) t->wake_ms = (uint64_t)(lx_now_ms() + timeout_ms);
        else if (lx_now_ms() >= (int64_t)t->wake_ms) { t->wake_ms = 0; return 0; }
    }
    lx_block(t, epoll_ready, 0);
    return 0;
}

// ---- eventfd / timerfd data -----------------------------------------------------------------------------------------

static int timer_expired(struct lx_event *e) {
    return e->next_ms && lx_now_ms() >= (int64_t)e->next_ms;
}

static int ev_read_ready(struct lx_task *t) {
    struct lx_file *f = file_of(t, (int64_t)t->block_arg);
    if (!f || f->kind != LXF_EVENT) return 1;
    return f->ev->kind == EV_EVENTFD ? f->ev->counter > 0 : timer_expired(f->ev);
}

int64_t lx_event_read(struct lx_task *t, struct lx_file *f, uint64_t uaddr, uint64_t len) {
    struct lx_event *e = f->ev;
    if (len < 8) return -LX_EINVAL;
    int fd = -1;
    for (int i = 0; i < LX_MAX_FDS; i++) if (t->files->fds[i] == f) { fd = i; break; }
    if (e->kind == EV_EVENTFD) {
        if (e->counter == 0) {
            if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
            lx_block(t, ev_read_ready, (uint64_t)fd);
            return 0;
        }
        uint64_t v = e->semaphore ? 1 : e->counter;
        e->counter -= v;
        return lx_copy_out(t, uaddr, &v, 8) ? -LX_EFAULT : 8;
    }
    if (e->kind == EV_TIMERFD) {
        if (!timer_expired(e)) {
            if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
            lx_block(t, ev_read_ready, (uint64_t)fd);
            return 0;
        }
        uint64_t n = 1;
        if (e->interval_ms) { n += (uint64_t)(lx_now_ms() - (int64_t)e->next_ms) / e->interval_ms; e->next_ms += n * e->interval_ms; }
        else e->next_ms = 0;
        return lx_copy_out(t, uaddr, &n, 8) ? -LX_EFAULT : 8;
    }
    if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
    return -LX_EAGAIN;                                                // inotify: never fires
}

int64_t lx_event_write(struct lx_task *t, struct lx_file *f, uint64_t uaddr, uint64_t len) {
    struct lx_event *e = f->ev;
    if (e->kind != EV_EVENTFD || len < 8) return -LX_EINVAL;
    uint64_t v;
    if (lx_copy_in(t, &v, uaddr, 8)) return -LX_EFAULT;
    e->counter += v;
    return 8;
}

int lx_event_ready(struct lx_task *t, struct lx_file *f, int want) {
    (void)t;
    struct lx_event *e = f->ev;
    int r = 0;
    if (e->kind == EV_EVENTFD) { if (e->counter) r |= 1; r |= 4; }
    else if (e->kind == EV_TIMERFD) { if (timer_expired(e)) r |= 1; }
    else if (e->kind == EV_EPOLL) {
        struct ep_event probe[1];
        (void)probe;
        for (int i = 0; i < EP_MAX; i++) if (e->ep[i].active && lx_fd_events(t, e->ep[i].f, (int)(e->ep[i].events & 0x1F))) { r |= 1; break; }
    }
    return r & (want | 0x18);
}

struct lx_itimerspec { int64_t isec, insec, vsec, vnsec; };

int64_t lx_sys_timerfd_settime(struct lx_task *t, int fd, int flags, uint64_t unew, uint64_t uold) {
    struct lx_file *f = file_of(t, fd);
    if (!f || f->kind != LXF_EVENT || f->ev->kind != EV_TIMERFD) return -LX_EBADF;
    struct lx_event *e = f->ev;
    if (uold) {
        struct lx_itimerspec o = { (int64_t)(e->interval_ms / 1000), (int64_t)(e->interval_ms % 1000) * 1000000, 0, 0 };
        int64_t rem = e->next_ms > (uint64_t)lx_now_ms() ? (int64_t)e->next_ms - lx_now_ms() : 0;
        o.vsec = rem / 1000; o.vnsec = (rem % 1000) * 1000000;
        if (lx_copy_out(t, uold, &o, sizeof o)) return -LX_EFAULT;
    }
    struct lx_itimerspec n;
    if (lx_copy_in(t, &n, unew, sizeof n)) return -LX_EFAULT;
    uint64_t val = (uint64_t)(n.vsec * 1000 + (n.vnsec + 999999) / 1000000);
    e->interval_ms = (uint64_t)(n.isec * 1000 + (n.insec + 999999) / 1000000);
    if (!val && !n.vsec && !n.vnsec) e->next_ms = 0;
    else if (flags & 1) { int64_t abs_ms = n.vsec * 1000 + n.vnsec / 1000000; e->next_ms = (uint64_t)abs_ms; }   // TFD_TIMER_ABSTIME (monotonic)
    else e->next_ms = (uint64_t)lx_now_ms() + (val ? val : 1);
    return 0;
}

int64_t lx_sys_timerfd_gettime(struct lx_task *t, int fd, uint64_t uval) {
    struct lx_file *f = file_of(t, fd);
    if (!f || f->kind != LXF_EVENT || f->ev->kind != EV_TIMERFD) return -LX_EBADF;
    struct lx_event *e = f->ev;
    struct lx_itimerspec o = { (int64_t)(e->interval_ms / 1000), (int64_t)(e->interval_ms % 1000) * 1000000, 0, 0 };
    int64_t rem = e->next_ms > (uint64_t)lx_now_ms() ? (int64_t)e->next_ms - lx_now_ms() : 0;
    o.vsec = rem / 1000; o.vnsec = (rem % 1000) * 1000000;
    return lx_copy_out(t, uval, &o, sizeof o);
}
