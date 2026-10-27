/* SPDX-License-Identifier: GPL-3.0-or-later
 * Client for the Savage Wind payload storage broker. One connection belongs
 * to one thread at a time; independent connections may be used concurrently.
 * The token is mode 0666 for the title uid: it is cooperative authentication,
 * not a secret from local homebrew. See protocol.h for the behavioral reference.
 */
#ifndef SAVAGE_STORAGE_CLIENT_H
#define SAVAGE_STORAGE_CLIENT_H
#include <stdint.h>
#include "protocol.h"

struct sb_client { int fd; uint32_t server_version; };

/* Initialize as { -1, 0 }, then connect; close before reconnecting. All calls
 * return a nonnegative result or negative errno. A transport/protocol failure
 * closes the connection. sb_connect_version permits a newer broker executable
 * to authenticate to an older one solely to issue sb_quit; ordinary callers
 * use sb_connect. sb_quit is accepted only for a strictly newer version.
 *
 * Paths are relative to SB_ROOT with existing parent directories; empty,
 * absolute, dot and dot-dot components, symlinks, multiply linked regular
 * files, and broker-owned names are rejected. mkdir creates one directory.
 * create creates or truncates a regular file; create_exclusive atomically
 * creates a new file or returns -EEXIST. pread and pwrite return bytes
 * transferred, which may be short; each request is at most SB_MAX_TRANSFER.
 * stat returns the file size through its output pointer. replace creates a
 * file if absent, or atomically replaces an existing regular file.
 * append uses one O_APPEND write and returns the bytes written (possibly short);
 * truncate changes the size, and fsync flushes the file. rename moves a regular
 * single-link file or a directory within the root; an existing destination must
 * have the same type, be single-link if regular, or be empty if a directory.
 * mkdir and rmdir affect one directory; rmdir only removes an empty directory.
 * Namespace changes sync the affected parent directories.
 *
 * list returns a byte count containing packed sb_list_entry headers followed
 * by raw, non-NUL-terminated names; use memcpy to read potentially unaligned
 * headers. It lists one directory only, excluding symlinks, multiply linked
 * files and broker-owned names. Pass "." to list the root. Insufficient capacity
 * returns -ENOSPC and no entries. Capacity is at most SB_MAX_TRANSFER.
 *
 * repair walks SB_ROOT on its existing filesystem, setting real directories to
 * 0777 and single-link regular files to 0666 without changing ownership.
 * Symlinks are skipped; special files, hardlinks, other filesystems, bounds and
 * filesystem errors produce a negative errno. Partial changes are not undone.
 * Stats include the root and are returned even on broker-reported failure;
 * a transport failure may leave them zero. */
int sb_connect(struct sb_client *client);
int sb_connect_version(struct sb_client *client, uint32_t version);
void sb_close(struct sb_client *client);
int sb_create(struct sb_client *client, const char *path);
int sb_create_exclusive(struct sb_client *client, const char *path);
int sb_pwrite(struct sb_client *client, const char *path, const void *data,
              uint32_t length, uint64_t offset);
int sb_pread(struct sb_client *client, const char *path, void *data,
             uint32_t length, uint64_t offset);
int sb_stat(struct sb_client *client, const char *path, uint64_t *size);
/* Replace atomically; the complete content is durable before rename. */
int sb_replace(struct sb_client *client, const char *path, const void *data,
               uint32_t length);
int sb_unlink(struct sb_client *client, const char *path);
int sb_mkdir(struct sb_client *client, const char *path);
int sb_append(struct sb_client *client, const char *path, const void *data,
              uint32_t length);
int sb_truncate(struct sb_client *client, const char *path, uint64_t size);
int sb_fsync(struct sb_client *client, const char *path);
int sb_rename(struct sb_client *client, const char *from, const char *to);
int sb_rmdir(struct sb_client *client, const char *path);
int sb_list(struct sb_client *client, const char *path, void *buffer, uint32_t capacity);
int sb_repair(struct sb_client *client, struct sb_repair_stats *stats);
int sb_quit(struct sb_client *client);
#endif
