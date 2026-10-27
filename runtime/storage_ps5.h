// PS5 persistent storage. All paths are relative to /data/savagewind.
#ifndef RT_STORAGE_PS5_H
#define RT_STORAGE_PS5_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int rt_storage_mkdirs(const char *path);
int rt_storage_mkdir(const char *path);
int rt_storage_create(const char *path);
int rt_storage_create_exclusive(const char *path);
ssize_t rt_storage_read(const char *path, void *data, size_t size, uint64_t offset);
ssize_t rt_storage_write(const char *path, const void *data, size_t size, uint64_t offset);
int rt_storage_append(const char *path, const void *data, size_t size);
int rt_storage_stat(const char *path, uint64_t *size);
int rt_storage_truncate(const char *path, uint64_t size);
int rt_storage_fsync(const char *path);
int rt_storage_rename(const char *from, const char *to);
int rt_storage_unlink(const char *path);
int rt_storage_rmdir(const char *path);
int rt_storage_remove_tree(const char *path);
int rt_storage_list(const char *path, void *buffer, uint32_t capacity);
int rt_storage_replace(const char *path, const void *data, size_t size);
#endif
