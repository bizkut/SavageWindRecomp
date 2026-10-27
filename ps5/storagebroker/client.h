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
 * create creates or truncates a regular file. pread and pwrite return bytes
 * transferred, which may be short; each request is at most SB_MAX_TRANSFER.
 * stat returns the file size through its output pointer. replace creates a
 * file if absent, or atomically replaces an existing regular file. */
int sb_connect(struct sb_client *client);
int sb_connect_version(struct sb_client *client, uint32_t version);
void sb_close(struct sb_client *client);
int sb_create(struct sb_client *client, const char *path);
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
int sb_quit(struct sb_client *client);
#endif
