#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <stdint.h>

#define FS_MAX_FILES 16
#define FS_MAX_NAME 32
// 32KB matches the PFS per-file cap (OKPFS1 runs are 64 sectors). VFS is the
// live API — files bigger than this truncate on write (same as before).
#define FS_MAX_SIZE 32768

struct fs_file {
    char name[FS_MAX_NAME];
    uint8_t data[FS_MAX_SIZE];
    int size;
    int used;
};

void fs_init(void);
void fs_install_persist(int (*sync_fn)(const char *),
                        int (*delete_fn)(const char *));
int fs_create(const char* name);
int fs_write(const char* name, const uint8_t* data, int len);
int fs_append(const char* name, const uint8_t* data, int len);
int fs_read(const char* name, uint8_t* buf, int max_len);
int fs_exists(const char* name);
int fs_delete(const char* name);
int fs_get_count(void);
const char* fs_get_name(int index);
int fs_get_size(const char* name);

#endif
