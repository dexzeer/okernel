#ifndef LXFS_H
#define LXFS_H

// The Linux subsystem's file system: an in-memory tree (directories, files, symlinks, device nodes) that the Linux
// system calls (lx_sys.c) work on. Its root is filled from a tar archive (a boot module) and by the system itself.

#include <stdint.h>

#define LXN_REG 1
#define LXN_DIR 2
#define LXN_CHR 3
#define LXN_LNK 4
#define LXN_SOCK 5

#define LX_NAME_MAX 255
#define LX_PATH_MAX 4096

struct lx_node {
    uint8_t  type;
    uint8_t  ro_data;            // data points into a boot module (copied on the first write)
    uint8_t  dead;               // unlinked while still open: freed with the last reference
    uint8_t  pad;
    uint16_t mode;               // permission bits (the file type comes from `type`)
    uint16_t nlink;
    uint32_t uid, gid, ino;
    uint64_t size;               // regular: bytes; symlink: target length; directory: 4096
    int64_t  mtime;              // seconds since 1970
    struct lx_node *parent, *child, *next;
    char *name;
    uint8_t *data;               // regular file contents
    uint32_t cap;                // allocated bytes of `data` (0 for a module-backed file)
    char *target;                // symlink
    uint32_t dev;                // character device: major << 8 | minor
    int refs;                    // open files and mappings
};

// Character device numbers (major << 8 | minor), as on Linux
#define LXDEV(maj, min) (((maj) << 8) | (min))
#define LXDEV_NULL    LXDEV(1, 3)
#define LXDEV_ZERO    LXDEV(1, 5)
#define LXDEV_FULL    LXDEV(1, 7)
#define LXDEV_RANDOM  LXDEV(1, 8)
#define LXDEV_URANDOM LXDEV(1, 9)
#define LXDEV_TTY     LXDEV(5, 0)
#define LXDEV_CONSOLE LXDEV(5, 1)
#define LXDEV_FB0     LXDEV(29, 0)
#define LXDEV_EVENT0  LXDEV(13, 64)
#define LXDEV_OKWIN   LXDEV(10, 200)

void lxfs_init(void);
struct lx_node *lxfs_root(void);

// Walk a path from `cwd` (absolute paths start at the root). follow_last: resolve a symlink in the last component.
// Returns 0 and sets *err to a negative errno on failure.
struct lx_node *lxfs_resolve(struct lx_node *cwd, const char *path, int follow_last, int *err);
// The directory that holds the last component of `path`, and that component's name (name[LX_NAME_MAX+1]).
int lxfs_resolve_parent(struct lx_node *cwd, const char *path, struct lx_node **dir, char *name);
// New node in `dir` (negative errno through *err). `mode` is the permission bits.
struct lx_node *lxfs_create(struct lx_node *dir, const char *name, int type, int mode, int *err);
int lxfs_symlink(struct lx_node *dir, const char *name, const char *target);
// Remove `name` from `dir`: files and symlinks (want_dir = 0) or an empty directory (want_dir = 1). 0 or -errno.
int lxfs_unlink(struct lx_node *dir, const char *name, int want_dir);
int lxfs_rename(struct lx_node *odir, const char *oname, struct lx_node *ndir, const char *nname);

int64_t lxfs_read(struct lx_node *n, uint64_t off, void *buf, uint64_t len);
int64_t lxfs_write(struct lx_node *n, uint64_t off, const void *buf, uint64_t len);
int lxfs_truncate(struct lx_node *n, uint64_t size);

// Directory entry `index` of a directory: 0 = ".", 1 = "..", then the children in creation order. 0 = past the end.
struct lx_node *lxfs_dir_entry(struct lx_node *dir, uint64_t index, const char **name);
// Absolute path of a node
void lxfs_path_of(struct lx_node *n, char *buf, int cap);

void lxfs_ref(struct lx_node *n);
void lxfs_unref(struct lx_node *n);

// Unpack a tar archive (ustar / GNU long names) under the root. Returns the number of entries, or -1.
int lxfs_load_tar(const uint8_t *data, uint32_t len);

#endif
