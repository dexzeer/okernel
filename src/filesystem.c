#include "filesystem.h"
#include "serial.h"

static struct fs_file files[FS_MAX_FILES];
static int file_count = 0;

void fs_init(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        files[i].used = 0;
        files[i].size = 0;
        files[i].name[0] = 0;
    }
    file_count = 0;
    serial_puts("[fs] initialized\n");
}

static int find_file(const char* name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used) {
            int match = 1;
            for (int j = 0; name[j] || files[i].name[j]; j++) {
                if (name[j] != files[i].name[j]) { match = 0; break; }
            }
            if (match) return i;
        }
    }
    return -1;
}

int fs_create(const char* name) {
    if (fs_exists(name)) return 0; // Already exists
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            files[i].used = 1;
            files[i].size = 0;
            int j = 0;
            while (name[j] && j < FS_MAX_NAME - 1) {
                files[i].name[j] = name[j];
                j++;
            }
            files[i].name[j] = 0;
            file_count++;
            serial_puts("[fs] created: ");
            serial_puts(name);
            serial_putchar('\n');
            return 1;
        }
    }
    return -1; // No space
}

int fs_write(const char* name, const uint8_t* data, int len) {
    int idx = find_file(name);
    if (idx < 0) {
        if (fs_create(name) <= 0) return -1;
        idx = find_file(name);
    }
    if (idx < 0) return -1;
    if (len > FS_MAX_SIZE) len = FS_MAX_SIZE;
    for (int i = 0; i < len; i++) files[idx].data[i] = data[i];
    files[idx].size = len;
    return len;
}

int fs_append(const char* name, const uint8_t* data, int len) {
    int idx = find_file(name);
    if (idx < 0) {
        idx = fs_create(name);
        if (idx <= 0) return -1;
        idx = find_file(name);
    }
    int remaining = FS_MAX_SIZE - files[idx].size;
    if (len > remaining) len = remaining;
    for (int i = 0; i < len; i++) {
        files[idx].data[files[idx].size + i] = data[i];
    }
    files[idx].size += len;
    return len;
}

int fs_read(const char* name, uint8_t* buf, int max_len) {
    int idx = find_file(name);
    if (idx < 0) return -1;
    int len = files[idx].size;
    if (len > max_len) len = max_len;
    for (int i = 0; i < len; i++) buf[i] = files[idx].data[i];
    return len;
}

int fs_exists(const char* name) {
    return find_file(name) >= 0;
}

int fs_delete(const char* name) {
    int idx = find_file(name);
    if (idx < 0) return 0;
    files[idx].used = 0;
    files[idx].size = 0;
    files[idx].name[0] = 0;
    file_count--;
    return 1;
}

int fs_get_count(void) {
    return file_count;
}

const char* fs_get_name(int index) {
    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used) {
            if (count == index) return files[i].name;
            count++;
        }
    }
    return 0;
}

int fs_get_size(const char* name) {
    int idx = find_file(name);
    if (idx < 0) return -1;
    return files[idx].size;
}
