#include "lxfs.h"
#include "lx.h"
#include "memory.h"
#include "serial.h"
#include "string.h"

extern int64_t kclock_epoch_ms(void);

static struct lx_node *root;
static uint32_t next_ino = 2;

static int64_t now_s(void) { return kclock_epoch_ms() / 1000; }

static char *dup_str(const char *s) {
    uint32_t n = (uint32_t)strlen(s);
    char *d = (char*)kmalloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static struct lx_node *node_new(int type, const char *name, int mode) {
    struct lx_node *n = (struct lx_node*)kcalloc(1, sizeof *n);
    if (!n) return 0;
    n->type = (uint8_t)type;
    n->mode = (uint16_t)mode;
    n->nlink = type == LXN_DIR ? 2 : 1;
    n->ino = next_ino++;
    n->mtime = now_s();
    n->name = dup_str(name);
    if (!n->name) { kfree(n); return 0; }
    if (type == LXN_DIR) n->size = 4096;
    return n;
}

void lxfs_init(void) {
    root = node_new(LXN_DIR, "", 0755);
    root->parent = root;
    serial_puts("[lxfs] root created\n");
}

struct lx_node *lxfs_root(void) { return root; }

int lxfs_case_insensitive;                                   // set while serving a Windows program

static struct lx_node *find_child(struct lx_node *dir, const char *name, uint32_t len) {
    for (struct lx_node *c = dir->child; c; c = c->next)
        if (strlen(c->name) == len && memcmp(c->name, name, len) == 0) return c;
    if (lxfs_case_insensitive)
        for (struct lx_node *c = dir->child; c; c = c->next) {
            if (strlen(c->name) != len) continue;
            uint32_t i = 0;
            while (i < len && (c->name[i] | 0x20) == (name[i] | 0x20) && ((c->name[i] | 0x20) < 'a' || (c->name[i] | 0x20) > 'z' ? c->name[i] == name[i] : 1)) i++;
            if (i == len) return c;
        }
    return 0;
}

struct lx_node *lxfs_resolve(struct lx_node *cwd, const char *path, int follow_last, int *err) {
    int dummy;
    if (!err) err = &dummy;
    if (!path || !*path) { *err = -LX_ENOENT; return 0; }
    struct lx_node *cur = path[0] == '/' ? root : (cwd ? cwd : root);
    int links = 0;
    char linkbuf[LX_PATH_MAX];
    const char *p = path;
    // Symlinks are expanded by splicing their target in front of the rest of the path (kept in `linkbuf`).
    char rest[LX_PATH_MAX];
    uint32_t pl = (uint32_t)strlen(p);
    if (pl >= LX_PATH_MAX) { *err = -LX_ENAMETOOLONG; return 0; }
    memcpy(rest, p, pl + 1);
    p = rest;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        uint32_t len = (uint32_t)(p - s);
        const char *after = p;
        while (*after == '/') after++;
        int last = (*after == 0);
        if (len > LX_NAME_MAX) { *err = -LX_ENAMETOOLONG; return 0; }
        if (cur->type != LXN_DIR) { *err = -LX_ENOTDIR; return 0; }
        struct lx_node *nx;
        if (len == 1 && s[0] == '.') nx = cur;
        else if (len == 2 && s[0] == '.' && s[1] == '.') nx = cur->parent ? cur->parent : cur;
        else {
            nx = find_child(cur, s, len);
            if (!nx) { *err = -LX_ENOENT; return 0; }
        }
        if (nx->type == LXN_LNK && (!last || follow_last)) {
            if (++links > 40) { *err = -LX_ELOOP; return 0; }
            // new path = target + "/" + the rest after this component
            uint32_t tl = (uint32_t)strlen(nx->target), al = (uint32_t)strlen(after);
            if (tl + 1 + al + 1 > LX_PATH_MAX) { *err = -LX_ENAMETOOLONG; return 0; }
            memcpy(linkbuf, nx->target, tl);
            linkbuf[tl] = '/';
            memcpy(linkbuf + tl + 1, after, al + 1);
            memcpy(rest, linkbuf, tl + 1 + al + 1);
            p = rest;
            if (nx->target[0] == '/') cur = root;      // a relative target is taken from `cur` (the link's directory)
            continue;
        }
        cur = nx;
        p = after;
    }
    return cur;
}

int lxfs_resolve_parent(struct lx_node *cwd, const char *path, struct lx_node **dir, char *name) {
    uint32_t n = (uint32_t)strlen(path);
    if (!n) return -LX_ENOENT;
    if (n >= LX_PATH_MAX) return -LX_ENAMETOOLONG;
    while (n > 1 && path[n - 1] == '/') n--;                 // trailing slashes
    uint32_t cut = n;
    while (cut > 0 && path[cut - 1] != '/') cut--;
    uint32_t nl = n - cut;
    if (nl == 0) return -LX_EINVAL;                           // "/" itself
    if (nl > LX_NAME_MAX) return -LX_ENAMETOOLONG;
    memcpy(name, path + cut, nl);
    name[nl] = 0;
    char dp[LX_PATH_MAX];
    if (cut == 0) { dp[0] = '.'; dp[1] = 0; }
    else { memcpy(dp, path, cut); dp[cut] = 0; }
    int err = 0;
    struct lx_node *d = lxfs_resolve(cwd, dp, 1, &err);
    if (!d) return err;
    if (d->type != LXN_DIR) return -LX_ENOTDIR;
    *dir = d;
    return 0;
}

struct lx_node *lxfs_create(struct lx_node *dir, const char *name, int type, int mode, int *err) {
    int dummy;
    if (!err) err = &dummy;
    if (dir->type != LXN_DIR) { *err = -LX_ENOTDIR; return 0; }
    uint32_t len = (uint32_t)strlen(name);
    if (!len || len > LX_NAME_MAX) { *err = -LX_ENAMETOOLONG; return 0; }
    if (find_child(dir, name, len)) { *err = -LX_EEXIST; return 0; }
    struct lx_node *n = node_new(type, name, mode);
    if (!n) { *err = -LX_ENOMEM; return 0; }
    n->parent = dir;
    // append (creation order)
    struct lx_node **pp = &dir->child;
    while (*pp) pp = &(*pp)->next;
    *pp = n;
    if (type == LXN_DIR) dir->nlink++;
    dir->mtime = now_s();
    return n;
}

int lxfs_symlink(struct lx_node *dir, const char *name, const char *target) {
    int err = 0;
    struct lx_node *n = lxfs_create(dir, name, LXN_LNK, 0777, &err);
    if (!n) return err;
    n->target = dup_str(target);
    if (!n->target) return -LX_ENOMEM;
    n->size = strlen(target);
    return 0;
}

static void node_free(struct lx_node *n) {
    if (!n->ro_data && n->data && n->type != LXN_SOCK) kfree(n->data);
    if (n->target) kfree(n->target);
    kfree(n->name);
    kfree(n);
}

void lxfs_ref(struct lx_node *n) { n->refs++; }
void lxfs_unref(struct lx_node *n) {
    if (--n->refs <= 0 && n->dead) node_free(n);
}

int lxfs_unlink(struct lx_node *dir, const char *name, int want_dir) {
    uint32_t len = (uint32_t)strlen(name);
    struct lx_node **pp = &dir->child;
    while (*pp && !(strlen((*pp)->name) == len && memcmp((*pp)->name, name, len) == 0)) pp = &(*pp)->next;
    struct lx_node *n = *pp;
    if (!n) return -LX_ENOENT;
    if (want_dir) {
        if (n->type != LXN_DIR) return -LX_ENOTDIR;
        if (n->child) return -LX_ENOTEMPTY;
        dir->nlink--;
    } else if (n->type == LXN_DIR) return -LX_EISDIR;
    *pp = n->next;
    dir->mtime = now_s();
    n->dead = 1;
    if (n->refs <= 0) node_free(n);
    return 0;
}

int lxfs_rename(struct lx_node *odir, const char *oname, struct lx_node *ndir, const char *nname) {
    uint32_t ol = (uint32_t)strlen(oname);
    struct lx_node **pp = &odir->child;
    while (*pp && !(strlen((*pp)->name) == ol && memcmp((*pp)->name, oname, ol) == 0)) pp = &(*pp)->next;
    struct lx_node *n = *pp;
    if (!n) return -LX_ENOENT;
    // refuse to move a directory into itself
    for (struct lx_node *a = ndir; ; a = a->parent) { if (a == n) return -LX_EINVAL; if (a == root || !a->parent) break; }
    uint32_t nl = (uint32_t)strlen(nname);
    struct lx_node *old = find_child(ndir, nname, nl);
    if (old == n) return 0;
    if (old) {
        if (old->type == LXN_DIR && n->type != LXN_DIR) return -LX_EISDIR;
        if (old->type != LXN_DIR && n->type == LXN_DIR) return -LX_ENOTDIR;
        int r = lxfs_unlink(ndir, nname, old->type == LXN_DIR);
        if (r) return r;
        pp = &odir->child;                                    // the list may have changed (same dir)
        while (*pp && *pp != n) pp = &(*pp)->next;
    }
    char *nn = dup_str(nname);
    if (!nn) return -LX_ENOMEM;
    *pp = n->next;
    n->next = 0;
    kfree(n->name);
    n->name = nn;
    n->parent = ndir;
    struct lx_node **tp = &ndir->child;
    while (*tp) tp = &(*tp)->next;
    *tp = n;
    if (n->type == LXN_DIR && odir != ndir) { odir->nlink--; ndir->nlink++; }
    return 0;
}

static int ensure_writable(struct lx_node *n, uint64_t need) {
    if (n->ro_data) {                                         // copy the module-backed contents out
        uint32_t cap = (uint32_t)(n->size > need ? n->size : need);
        if (cap < 64) cap = 64;
        uint8_t *d = (uint8_t*)kmalloc(cap);
        if (!d) return -LX_ENOMEM;
        memcpy(d, n->data, (uint32_t)n->size);
        n->data = d; n->cap = cap; n->ro_data = 0;
    }
    if (need > n->cap) {
        uint64_t cap = n->cap ? n->cap : 64;
        while (cap < need) cap *= 2;
        if (cap > 0x7FFFFFF0u) return -LX_EFBIG;
        uint8_t *d = (uint8_t*)krealloc(n->data, (uint32_t)cap);
        if (!d) return -LX_ENOMEM;
        n->data = d; n->cap = (uint32_t)cap;
    }
    return 0;
}

int64_t lxfs_read(struct lx_node *n, uint64_t off, void *buf, uint64_t len) {
    if (n->type != LXN_REG) return -LX_EINVAL;
    if (off >= n->size) return 0;
    if (len > n->size - off) len = n->size - off;
    memcpy(buf, n->data + off, (uint32_t)len);
    return (int64_t)len;
}

int64_t lxfs_write(struct lx_node *n, uint64_t off, const void *buf, uint64_t len) {
    if (n->type != LXN_REG) return -LX_EINVAL;
    if (off + len > 0x7FFFFFF0u) return -LX_EFBIG;
    int r = ensure_writable(n, off + len);
    if (r) return r;
    if (off > n->size) memset(n->data + n->size, 0, (uint32_t)(off - n->size));     // a hole reads as zeros
    memcpy(n->data + off, buf, (uint32_t)len);
    if (off + len > n->size) n->size = off + len;
    n->mtime = now_s();
    return (int64_t)len;
}

int lxfs_truncate(struct lx_node *n, uint64_t size) {
    if (n->type != LXN_REG) return -LX_EINVAL;
    if (size > n->size) {
        int r = ensure_writable(n, size);
        if (r) return r;
        memset(n->data + n->size, 0, (uint32_t)(size - n->size));
    }
    n->size = size;
    n->mtime = now_s();
    return 0;
}

struct lx_node *lxfs_dir_entry(struct lx_node *dir, uint64_t index, const char **name) {
    if (index == 0) { *name = "."; return dir; }
    if (index == 1) { *name = ".."; return dir->parent ? dir->parent : dir; }
    uint64_t i = 2;
    for (struct lx_node *c = dir->child; c; c = c->next, i++)
        if (i == index) { *name = c->name; return c; }
    return 0;
}

void lxfs_path_of(struct lx_node *n, char *buf, int cap) {
    char tmp[LX_PATH_MAX];
    int pos = LX_PATH_MAX - 1;
    tmp[pos] = 0;
    if (n == root) { tmp[--pos] = '/'; }
    for (struct lx_node *c = n; c != root && c; c = c->parent) {
        int l = (int)strlen(c->name);
        if (pos - l - 1 < 0) break;
        pos -= l;
        memcpy(tmp + pos, c->name, (uint32_t)l);
        tmp[--pos] = '/';
    }
    int l = (int)strlen(tmp + pos);
    if (l >= cap) l = cap - 1;
    memcpy(buf, tmp + pos, (uint32_t)l);
    buf[l] = 0;
}

// ---- tar ---------------------------------------------------------------------------------------------------------

static uint64_t tar_oct(const char *p, int n) {
    uint64_t v = 0;
    int i = 0;
    while (i < n && p[i] == ' ') i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (uint64_t)(p[i] - '0');
    return v;
}

// mkdir -p for the directory part of `path`; returns the directory node
static struct lx_node *mkdirs(const char *path, uint32_t len) {
    struct lx_node *cur = root;
    uint32_t i = 0;
    while (i < len) {
        while (i < len && path[i] == '/') i++;
        uint32_t s = i;
        while (i < len && path[i] != '/') i++;
        uint32_t l = i - s;
        if (!l || (l == 1 && path[s] == '.')) continue;
        struct lx_node *c = find_child(cur, path + s, l);
        if (!c) {
            char nm[LX_NAME_MAX + 1];
            if (l > LX_NAME_MAX) return 0;
            memcpy(nm, path + s, l); nm[l] = 0;
            int e = 0;
            c = lxfs_create(cur, nm, LXN_DIR, 0755, &e);
            if (!c) return 0;
        }
        if (c->type != LXN_DIR) return 0;
        cur = c;
    }
    return cur;
}

int lxfs_load_tar(const uint8_t *data, uint32_t len) {
    int count = 0;
    char longname[LX_PATH_MAX];
    int have_long = 0;
    for (uint32_t off = 0; off + 512 <= len; ) {
        const char *h = (const char*)data + off;
        if (h[0] == 0) break;                                 // end of archive
        uint64_t size = tar_oct(h + 124, 12);
        int mode = (int)tar_oct(h + 100, 8) & 07777;
        char type = h[156];
        char path[LX_PATH_MAX];
        uint32_t pl = 0;
        if (have_long) { uint32_t n = (uint32_t)strlen(longname); memcpy(path, longname, n + 1); pl = n; have_long = 0; }
        else {
            if (h[345]) { for (int i = 0; i < 155 && h[345 + i]; i++) path[pl++] = h[345 + i]; path[pl++] = '/'; }
            for (int i = 0; i < 100 && h[i]; i++) path[pl++] = h[i];
            path[pl] = 0;
        }
        const uint8_t *body = data + off + 512;
        uint32_t adv = 512 + (uint32_t)((size + 511) & ~(uint64_t)511);
        if (off + 512 + size > len) { serial_puts("[lxfs] tar: truncated archive\n"); return -1; }
        if (type == 'L') {                                    // GNU long name: the body is the next entry's path
            uint32_t n = size < LX_PATH_MAX - 1 ? (uint32_t)size : LX_PATH_MAX - 1;
            memcpy(longname, body, n); longname[n] = 0; have_long = 1;
            off += adv;
            continue;
        }
        while (pl > 1 && path[pl - 1] == '/') path[--pl] = 0;
        if (pl && !(pl == 1 && path[0] == '.') && !(pl == 2 && path[0] == '.' && path[1] == '/')) {
            uint32_t cut = pl;
            while (cut > 0 && path[cut - 1] != '/') cut--;
            struct lx_node *dir = mkdirs(path, cut);
            const char *base = path + cut;
            if (dir && *base) {
                int e = 0;
                struct lx_node *n = 0;
                if (type == '5') {
                    n = find_child(dir, base, (uint32_t)strlen(base));
                    if (!n) n = lxfs_create(dir, base, LXN_DIR, mode, &e);
                    else n->mode = (uint16_t)mode;
                } else if (type == '2') {
                    char target[101];
                    int i = 0;
                    for (; i < 100 && h[157 + i]; i++) target[i] = h[157 + i];
                    target[i] = 0;
                    if (lxfs_symlink(dir, base, target) == 0) n = find_child(dir, base, (uint32_t)strlen(base));
                } else if (type == '0' || type == 0 || type == '7') {
                    n = lxfs_create(dir, base, LXN_REG, mode, &e);
                    if (n) { n->data = (uint8_t*)body; n->size = size; n->ro_data = 1; }
                }
                if (n) { n->uid = (uint32_t)tar_oct(h + 108, 8); n->gid = (uint32_t)tar_oct(h + 116, 8); n->mtime = (int64_t)tar_oct(h + 136, 12); count++; }
            }
        }
        off += adv;
    }
    serial_printf("[lxfs] tar: %d entries\n", count);
    return count;
}
