// xokai: server main loop, client connections, output queues, resource table, atoms.
#include "xsrv.h"

#define SYS_poll 7
#define SYS_socket 41
#define SYS_bind 49
#define SYS_listen 50
#define SYS_mkdir 83
#define SYS_unlink 87
#define SYS_accept4 288

int okfd = -1;
struct client *clients[MAXC];
struct client *cc;
static u64 start_ms;

u32 srv_time(void) { return (u32)(now_ms() - start_ms) + 1; }

int okw_ioctl(unsigned req, void *arg) { return (int)sc3(16, okfd, req, (i64)arg); }

// ---- resources ----------------------------------------------------------------------------------------------------------------------------
struct rnode { u32 id; int kind; void *p; struct rnode *next; };
#define RBUCKETS 4096
static struct rnode *rtab[RBUCKETS];

void *res_get(u32 id, int kind) {
    for (struct rnode *n = rtab[(id * 2654435761u) >> 20 & (RBUCKETS - 1)]; n; n = n->next) if (n->id == id) return n->kind == kind ? n->p : 0;
    return 0;
}

int res_free_id(u32 id) {
    for (struct rnode *n = rtab[(id * 2654435761u) >> 20 & (RBUCKETS - 1)]; n; n = n->next) if (n->id == id) return 0;
    return 1;
}

void res_put(u32 id, int kind, void *p) {
    struct rnode *n = (struct rnode*)xmalloc(sizeof *n);
    int h = (id * 2654435761u) >> 20 & (RBUCKETS - 1);
    n->id = id; n->kind = kind; n->p = p; n->next = rtab[h]; rtab[h] = n;
}

void res_del(u32 id) {
    for (struct rnode **pp = &rtab[(id * 2654435761u) >> 20 & (RBUCKETS - 1)]; *pp; pp = &(*pp)->next)
        if ((*pp)->id == id) { struct rnode *d = *pp; *pp = d->next; xfree(d); return; }
}

struct win *win_of(u32 id) { return (struct win*)res_get(id, RES_WINDOW); }

// frees what a client left behind (pixmaps, GCs, fonts, cursors)
static void res_sweep(struct client *c) {
    for (int b = 0; b < RBUCKETS; b++) {
        for (struct rnode **pp = &rtab[b]; *pp;) {
            struct rnode *n = *pp;
            if ((n->id & ~0x1FFFFFu) != c->base || n->kind == RES_WINDOW) { pp = &n->next; continue; }
            *pp = n->next;
            if (n->kind == RES_PIXMAP) { struct pix *p = (struct pix*)n->p; xfree(p->px); xfree(p); }
            else if (n->kind == RES_GC) { struct gc *g = (struct gc*)n->p; xfree(g->clip); xfree(g); }
            else if (n->kind == RES_FONT) font_unref((struct font*)n->p);
            else xfree(n->p);
            xfree(n);
        }
    }
}

// ---- atoms ------------------------------------------------------------------------------------------------------------------------------------
static const char *predef[] = { 0, "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL", "COLORMAP", "CURSOR", "CUT_BUFFER0", "CUT_BUFFER1",
    "CUT_BUFFER2", "CUT_BUFFER3", "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6", "CUT_BUFFER7", "DRAWABLE", "FONT", "INTEGER", "PIXMAP", "POINT",
    "RECTANGLE", "RESOURCE_MANAGER", "RGB_COLOR_MAP", "RGB_BEST_MAP", "RGB_BLUE_MAP", "RGB_DEFAULT_MAP", "RGB_GRAY_MAP", "RGB_GREEN_MAP",
    "RGB_RED_MAP", "STRING", "VISUALID", "WINDOW", "WM_COMMAND", "WM_HINTS", "WM_CLIENT_MACHINE", "WM_ICON_NAME", "WM_ICON_SIZE", "WM_NAME",
    "WM_NORMAL_HINTS", "WM_SIZE_HINTS", "WM_ZOOM_HINTS", "MIN_SPACE", "NORM_SPACE", "MAX_SPACE", "END_SPACE", "SUPERSCRIPT_X", "SUPERSCRIPT_Y",
    "SUBSCRIPT_X", "SUBSCRIPT_Y", "UNDERLINE_POSITION", "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT", "STRIKEOUT_DESCENT", "ITALIC_ANGLE",
    "X_HEIGHT", "QUAD_WIDTH", "WEIGHT", "POINT_SIZE", "RESOLUTION", "COPYRIGHT", "NOTICE", "FONT_NAME", "FAMILY_NAME", "FULL_NAME", "CAP_HEIGHT",
    "WM_CLASS", "WM_TRANSIENT_FOR" };
#define NPREDEF 69

#define MAXATOMS 8192
static char *atom_names[MAXATOMS];
static u32 natoms = 1;
u32 a_wm_protocols, a_wm_delete, a_utf8, a_net_wm_name, a_clipboard, a_targets;

u32 atom_intern(const char *name, int len, int create) {
    for (u32 i = 1; i < natoms; i++) {
        const char *a = atom_names[i];
        int k = 0;
        while (k < len && a[k] == name[k]) k++;
        if (k == len && !a[k]) return i;
    }
    if (!create || natoms >= MAXATOMS) return 0;
    atom_names[natoms] = xstrndup(name, (u64)len);
    return natoms++;
}

const char *atom_name(u32 a) { return a > 0 && a < natoms ? atom_names[a] : 0; }

static void atoms_init(void) {
    for (int i = 1; i < NPREDEF; i++) atom_intern(predef[i], (int)strlen(predef[i]), 1);
    a_wm_protocols = atom_intern("WM_PROTOCOLS", 12, 1);
    a_wm_delete = atom_intern("WM_DELETE_WINDOW", 16, 1);
    a_utf8 = atom_intern("UTF8_STRING", 11, 1);
    a_net_wm_name = atom_intern("_NET_WM_NAME", 12, 1);
    a_clipboard = atom_intern("CLIPBOARD", 9, 1);
    a_targets = atom_intern("TARGETS", 7, 1);
}

// ---- output ---------------------------------------------------------------------------------------------------------------------------------------
u8 *out_reserve(struct client *c, u32 n) {
    if (c->outlen + n > c->outcap) {
        u32 nc = c->outcap ? c->outcap : 4096;
        while (nc < c->outlen + n) nc *= 2;
        c->out = (u8*)xrealloc(c->out, nc);
        c->outcap = nc;
    }
    u8 *p = c->out + c->outlen;
    memset(p, 0, n);
    c->outlen += n;
    return p;
}

void reply_start(struct client *c, u8 *r, int data, u32 extra_bytes) {
    r[0] = 1; r[1] = (u8)data;
    p16(r, 2, c->seq);
    p32(r, 4, pad4(extra_bytes) / 4);
}

void send_error(struct client *c, int code, u32 val, int major, int minor) {
    u8 *e = out_reserve(c, 32);
    e[0] = 0; e[1] = (u8)code;
    p16(e, 2, c->seq); p32(e, 4, val); p16(e, 8, (u32)minor); e[10] = (u8)major;
}

void ev_to_client(struct client *c, const u8 *ev) {
    if (c->dead) return;
    u8 *e = out_reserve(c, 32);
    memcpy(e, ev, 32);
    if ((e[0] & 0x7F) != 11) p16(e, 2, c->seq);                          // KeymapNotify has no sequence number
}

static void client_write(struct client *c) {
    while (c->outoff < c->outlen) {
        i64 n = sys_write(c->fd, c->out + c->outoff, c->outlen - c->outoff);
        if (n == -11) return;                                              // EAGAIN: try again when writable
        if (n <= 0) { c->dead = 1; return; }
        c->outoff += (u32)n;
    }
    c->outoff = c->outlen = 0;
}

void flush_clients(void) {
    for (struct win *t = root->first; t; t = t->next) top_flush(t);
    for (int i = 1; i < MAXC; i++) if (clients[i] && clients[i]->outlen) client_write(clients[i]);
}

// ---- clients --------------------------------------------------------------------------------------------------------------------------------------
static void client_free(struct client *c) {
    LOG("xokai: client %d gone\n", c->idx);
    win_client_gone(c);
    input_client_gone(c);
    sel_client_gone(c);
    res_sweep(c);
    sc1(SYS_close, c->fd);
    clients[c->idx] = 0;
    xfree(c->in); xfree(c->out); xfree(c);
}

static void client_read(struct client *c) {
    for (;;) {
        if (c->inlen + 65536 > c->incap) {
            u32 nc = c->incap ? c->incap : 65536;
            while (nc < c->inlen + 65536) nc *= 2;
            c->in = (u8*)xrealloc(c->in, nc);
            c->incap = nc;
        }
        i64 n = sys_read(c->fd, c->in + c->inlen, 65536);
        if (n == -11) break;
        if (n <= 0) { c->dead = 1; return; }
        c->inlen += (u32)n;
        if (n < 65536) break;
    }
    u32 off = 0;
    while (!c->dead) {
        u32 left = c->inlen - off;
        if (c->state == 0) {
            if (left < 12) break;
            u32 need = 12 + pad4(g16(c->in + off, 6)) + pad4(g16(c->in + off, 8));
            if (left < need) break;
            req_setup(c, c->in + off, need);
            off += need;
            continue;
        }
        if (left < 4) break;
        u32 len = g16(c->in + off, 2) * 4;
        if (len == 0) { send_error(c, ER_Length, 0, c->in[off], 0); c->seq++; off += 4; continue; }
        if (left < len) break;
        c->seq++;
        cc = c;
        req_dispatch(c, c->in + off, len);
        off += len;
    }
    if (off) { memmove(c->in, c->in + off, c->inlen - off); c->inlen -= off; }
}

static int listen_socket(void) {
    sc2(SYS_mkdir, (i64)"/tmp", 0777);
    sc2(SYS_mkdir, (i64)"/tmp/.X11-unix", 0777);
    sc1(SYS_unlink, (i64)"/tmp/.X11-unix/X0");
    int fd = (int)sc3(SYS_socket, 1, 1, 0);
    if (fd < 0) return fd;
    struct { u16 fam; char path[108]; } sa;
    memset(&sa, 0, sizeof sa);
    sa.fam = 1;
    const char *p = "/tmp/.X11-unix/X0";
    for (int i = 0; p[i]; i++) sa.path[i] = p[i];
    if (sc3(SYS_bind, fd, (i64)&sa, 2 + (i64)strlen(p) + 1) < 0) { LOG("xokai: bind failed\n"); return -1; }
    if (sc2(SYS_listen, fd, 16) < 0) return -1;
    return fd;
}

struct pfd { int fd; short events, revents; };

__attribute__((force_align_arg_pointer)) void _start(void) {
    start_ms = now_ms();
    sc0(112);                                                              // setsid: leave the terminal the shell started us on
    okfd = (int)sc3(SYS_open, (i64)"/dev/okwin", 2, 0);
    if (okfd < 0) { puts_fd(2, "xokai: no /dev/okwin\n"); sys_exit(1); }
    atoms_init();
    input_init();
    win_init();
    int lfd = listen_socket();
    if (lfd < 0) { puts_fd(2, "xokai: cannot listen\n"); sys_exit(1); }
    LOG("xokai: display :0 ready (%dx%d)\n", screen_w, screen_h);
    for (;;) {
        struct pfd pf[MAXC + 2];
        int map[MAXC + 2], n = 0;
        pf[n].fd = lfd; pf[n].events = 1; pf[n].revents = 0; map[n++] = -1;
        pf[n].fd = okfd; pf[n].events = 1; pf[n].revents = 0; map[n++] = -2;
        for (int i = 1; i < MAXC; i++)
            if (clients[i]) { pf[n].fd = clients[i]->fd; pf[n].events = (short)(1 | (clients[i]->outlen ? 4 : 0)); pf[n].revents = 0; map[n++] = i; }
        i64 r = sc3(SYS_poll, (i64)pf, n, -1);
        if (r < 0) continue;
        for (int k = 0; k < n; k++) {
            if (!pf[k].revents) continue;
            if (map[k] == -1) {
                int fd = (int)sc4(SYS_accept4, lfd, 0, 0, 0x800);
                if (fd < 0) continue;
                int idx = 1;
                while (idx < MAXC && clients[idx]) idx++;
                if (idx >= MAXC) { sc1(SYS_close, fd); continue; }
                struct client *c = (struct client*)xcalloc(sizeof *c);
                c->fd = fd; c->idx = idx; c->base = (u32)idx << 21;
                clients[idx] = c;
            } else if (map[k] == -2) input_poll();
            else {
                struct client *c = clients[map[k]];
                if (!c) continue;
                if (pf[k].revents & 4) client_write(c);
                if (pf[k].revents & (1 | 8 | 16)) client_read(c);
            }
        }
        flush_clients();
        for (int i = 1; i < MAXC; i++) if (clients[i] && clients[i]->dead) client_free(clients[i]);
        flush_clients();
    }
}
