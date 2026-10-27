// PS5 persistent storage backend; independent of SDL and the game runtime.
#include "storage_ps5.h"
#include "../ps5/storagebroker/client.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int connect_client(struct sb_client *c) {
    *c = (struct sb_client){-1, 0};
    return sb_connect(c);
}
#define CALL1(name, api) \
int name(const char *path) { struct sb_client c; int r = connect_client(&c); \
    if (r >= 0) r = api(&c, path); sb_close(&c); return r; }
CALL1(rt_storage_create, sb_create)
CALL1(rt_storage_create_exclusive, sb_create_exclusive)
CALL1(rt_storage_mkdir, sb_mkdir)
CALL1(rt_storage_fsync, sb_fsync)
CALL1(rt_storage_unlink, sb_unlink)
CALL1(rt_storage_rmdir, sb_rmdir)

int rt_storage_stat(const char *path, uint64_t *size) {
    struct sb_client c; int r = connect_client(&c);
    if (r >= 0) r = sb_stat(&c, path, size);
    sb_close(&c); return r;
}
int rt_storage_truncate(const char *path, uint64_t size) {
    struct sb_client c; int r = connect_client(&c);
    if (r >= 0) r = sb_truncate(&c, path, size);
    sb_close(&c); return r;
}
int rt_storage_rename(const char *from, const char *to) {
    struct sb_client c; int r = connect_client(&c);
    if (r >= 0) r = sb_rename(&c, from, to);
    sb_close(&c); return r;
}
int rt_storage_list(const char *path, void *buffer, uint32_t capacity) {
    struct sb_client c; int r = connect_client(&c);
    if (r >= 0) r = sb_list(&c, path, buffer, capacity);
    sb_close(&c); return r;
}
int rt_storage_remove_tree(const char *path) {
    uint32_t capacity = 4096;
    void *buffer = NULL;
    int r;
    for (;;) {
        void *next = realloc(buffer, capacity);
        if (!next) { free(buffer); return -ENOMEM; }
        buffer = next;
        r = rt_storage_list(path, buffer, capacity);
        if (r != -ENOSPC || capacity == SB_MAX_TRANSFER) break;
        capacity = capacity > SB_MAX_TRANSFER / 2 ? SB_MAX_TRANSFER : capacity * 2;
    }
    if (r < 0) { free(buffer); return r; }
    size_t pos = 0, length = (size_t)r;
    while (pos < length) {
        struct sb_list_entry entry;
        if (length - pos < sizeof entry) { r = -EPROTO; break; }
        memcpy(&entry, (char *)buffer + pos, sizeof entry);
        pos += sizeof entry;
        if (!entry.name_length || entry.name_length > length - pos) { r = -EPROTO; break; }
        char child[SB_MAX_PATH];
        int n = snprintf(child, sizeof child, "%s/%.*s", path, (int)entry.name_length, (char *)buffer + pos);
        if (n < 0 || (size_t)n >= sizeof child) { r = -ENAMETOOLONG; break; }
        pos += entry.name_length;
        r = entry.type == SB_ENTRY_DIRECTORY ? rt_storage_remove_tree(child) :
            entry.type == SB_ENTRY_REGULAR ? rt_storage_unlink(child) : -EPROTO;
        if (r < 0) break;
    }
    free(buffer);
    return r < 0 ? r : rt_storage_rmdir(path);
}
int rt_storage_mkdirs(const char *path) {
    size_t len = strlen(path);
    if (!len || len >= SB_MAX_PATH) return -EINVAL;
    char tmp[SB_MAX_PATH]; memcpy(tmp, path, len + 1);
    struct sb_client c; int r = connect_client(&c);
    if (r < 0) return r;
    for (char *p = tmp; ; p++) {
        if (*p != '/' && *p != '\0') continue;
        char ch = *p; *p = '\0';
        r = sb_mkdir(&c, tmp);
        *p = ch;
        if (r < 0 && r != -EEXIST) break;
        if (!ch) { r = 0; break; }
    }
    sb_close(&c); return r;
}
static ssize_t transfer(const char *path, void *data, size_t size, uint64_t offset, int write_data) {
    struct sb_client c; int r = connect_client(&c);
    if (r < 0) return r;
    size_t done = 0;
    while (done < size) {
        uint32_t n = size - done > SB_MAX_TRANSFER ? SB_MAX_TRANSFER : (uint32_t)(size - done);
        r = write_data ? sb_pwrite(&c, path, (char *)data + done, n, offset + done)
                       : sb_pread(&c, path, (char *)data + done, n, offset + done);
        if (r < 0 || r == 0) break;
        done += (size_t)r;
        if (!write_data && (uint32_t)r < n) break;
    }
    sb_close(&c);
    return r < 0 ? r : (ssize_t)done;
}
ssize_t rt_storage_read(const char *path, void *data, size_t size, uint64_t offset) {
    return transfer(path, data, size, offset, 0);
}
ssize_t rt_storage_write(const char *path, const void *data, size_t size, uint64_t offset) {
    return transfer(path, (void *)data, size, offset, 1);
}
int rt_storage_append(const char *path, const void *data, size_t size) {
    struct sb_client c; int r = connect_client(&c);
    if (r < 0) return r;
    size_t done = 0;
    while (done < size) {
        uint32_t n = size - done > SB_MAX_TRANSFER ? SB_MAX_TRANSFER : (uint32_t)(size - done);
        r = sb_append(&c, path, (const char *)data + done, n);
        if (r <= 0) { if (!r) r = -EIO; break; }
        done += (size_t)r;
    }
    sb_close(&c); return r < 0 ? r : 0;
}
int rt_storage_replace(const char *path, const void *data, size_t size) {
    struct sb_client c; int r = connect_client(&c);
    if (r < 0) return r;
    if (size <= SB_MAX_TRANSFER) {
        r = sb_replace(&c, path, data, (uint32_t)size);
        sb_close(&c); return r;
    }
    static _Atomic unsigned counter;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts)) { r = -errno; sb_close(&c); return r; }
    char tmp[SB_MAX_PATH];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp-%lx-%lx-%lx-%x", path,
                     (unsigned long)getpid(), (unsigned long)ts.tv_sec,
                     (unsigned long)ts.tv_nsec, atomic_fetch_add(&counter, 1));
    if (n < 0 || (size_t)n >= sizeof tmp) { sb_close(&c); return -ENAMETOOLONG; }
    r = sb_create_exclusive(&c, tmp);
    if (r < 0) { sb_close(&c); return r; }
    size_t done = 0;
    while (done < size) {
        uint32_t len = size - done > SB_MAX_TRANSFER ? SB_MAX_TRANSFER : (uint32_t)(size - done);
        r = sb_pwrite(&c, tmp, (const char *)data + done, len, done);
        if (r <= 0) { if (!r) r = -EIO; break; }
        done += (size_t)r;
    }
    if (r >= 0) r = sb_fsync(&c, tmp);
    if (r >= 0) r = sb_rename(&c, tmp, path);
    if (r < 0) sb_unlink(&c, tmp);
    sb_close(&c); return r;
}
