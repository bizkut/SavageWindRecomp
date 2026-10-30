/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "client.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int io_all(int fd, void *buffer, size_t length, int writing)
{
    unsigned char *p = buffer;
    while (length) {
        ssize_t n = writing ? send(fd, p, length, MSG_NOSIGNAL) : recv(fd, p, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n ? -errno : -EPIPE;
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

void sb_close(struct sb_client *c)
{
    if (c && c->fd >= 0) close(c->fd);
    if (c) { c->fd = -1; c->server_version = 0; }
}

int sb_connect_version(struct sb_client *c, uint32_t version)
{
    struct sb_hello hello = { SB_MAGIC, version, {0} };
    struct sb_reply reply;
    struct sockaddr_in addr = {0};
    size_t done = 0;
    int fd, one = 1, rc;
    if (!c || !version || c->fd != -1) return -EINVAL;
    fd = open(SB_ROOT "/" SB_TOKEN_NAME, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -errno;
    while (done < sizeof(hello.token)) {
        ssize_t n = read(fd, hello.token + done, sizeof(hello.token) - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { rc = n ? -errno : -EIO; close(fd); return rc; }
        done += (size_t)n;
    }
    close(fd);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    c->fd = fd;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one))) {
        rc = -errno; goto fail;
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SB_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) { rc = -errno; goto fail; }
    if ((rc = io_all(fd, &hello, sizeof(hello), 1)) ||
        (rc = io_all(fd, &reply, sizeof(reply), 0))) goto fail;
    if (reply.magic != SB_MAGIC || reply.reserved || !reply.version) { rc = -EPROTO; goto fail; }
    c->server_version = reply.version;
    if (reply.result && !(reply.result == -EPROTONOSUPPORT && version > reply.version)) {
        rc = reply.result < 0 ? reply.result : -EPROTO; goto fail;
    }
    if (!reply.result && version != reply.version) { rc = -EPROTO; goto fail; }
    return 0;
fail:
    sb_close(c);
    return rc;
}
int sb_connect(struct sb_client *c) { return sb_connect_version(c, SB_VERSION); }

static int request(struct sb_client *c, uint32_t op, const char *path,
                   void *data, uint32_t length, uint64_t offset, uint64_t *value)
{
    struct sb_request req = { SB_MAGIC, op, 0, length, offset };
    struct sb_reply reply;
    int rc;
    if (!c || c->fd < 0 || length > SB_MAX_TRANSFER || (length && !data &&
        (op == SB_READ || op == SB_PACKAGE_READ || op == SB_WRITE || op == SB_REPLACE ||
         op == SB_APPEND || op == SB_RENAME || op == SB_LIST || op == SB_INSTALL_HOME_ART))) return -EINVAL;
    if ((op == SB_READ || op == SB_PACKAGE_READ || op == SB_WRITE) &&
        (offset > INT64_MAX || length > (uint64_t)INT64_MAX - offset)) return -EINVAL;
    if (op == SB_TRUNCATE && offset > INT64_MAX) return -EINVAL;
    if (path) {
        size_t n = strnlen(path, SB_MAX_PATH + 1u);
        if (!n || n > SB_MAX_PATH) return -EINVAL;
        req.path_length = (uint32_t)n;
    } else if (op != SB_QUIT && op != SB_REPAIR && op != SB_PACKAGE_READ &&
               op != SB_INSTALL_HOME_ART && op != SB_REMOVE_HOME_BACKGROUNDS) return -EINVAL;
    if ((rc = io_all(c->fd, &req, sizeof(req), 1)) ||
        (req.path_length && (rc = io_all(c->fd, (void *)path, req.path_length, 1))) ||
        ((op == SB_WRITE || op == SB_REPLACE || op == SB_APPEND || op == SB_RENAME ||
          op == SB_INSTALL_HOME_ART) && length && (rc = io_all(c->fd, data, length, 1))) ||
        (rc = io_all(c->fd, &reply, sizeof(reply), 0))) goto fail;
    if (reply.magic != SB_MAGIC || reply.reserved || reply.version != c->server_version ||
        (reply.result > 0 && op != SB_READ && op != SB_PACKAGE_READ && op != SB_WRITE &&
         op != SB_APPEND && op != SB_LIST) ||
        ((op == SB_READ || op == SB_PACKAGE_READ || op == SB_WRITE || op == SB_APPEND || op == SB_LIST) &&
         reply.result > (int32_t)length)) {
        rc = -EPROTO; goto fail;
    }
    if ((op == SB_READ || op == SB_PACKAGE_READ || op == SB_LIST) && reply.result > 0 &&
        (rc = io_all(c->fd, data, (size_t)reply.result, 0))) goto fail;
    if (op == SB_REPAIR && (rc = io_all(c->fd, data, sizeof(struct sb_repair_stats), 0)))
        goto fail;
    if (value && reply.result == 0) *value = reply.value;
    return reply.result;
fail:
    sb_close(c);
    return rc;
}
int sb_create(struct sb_client *c, const char *p) { return request(c, SB_CREATE, p, NULL, 0, 0, NULL); }
int sb_create_exclusive(struct sb_client *c, const char *p)
{ return request(c, SB_CREATE_EXCLUSIVE, p, NULL, 0, 0, NULL); }
int sb_pwrite(struct sb_client *c, const char *p, const void *d, uint32_t n, uint64_t o)
{ return request(c, SB_WRITE, p, (void *)d, n, o, NULL); }
int sb_pread(struct sb_client *c, const char *p, void *d, uint32_t n, uint64_t o)
{ return request(c, SB_READ, p, d, n, o, NULL); }
int sb_package_open(struct sb_client *c, const char *p)
{ return request(c, SB_PACKAGE_OPEN, p, NULL, 0, 0, NULL); }
int sb_package_read(struct sb_client *c, void *d, uint32_t n, uint64_t o)
{ return request(c, SB_PACKAGE_READ, NULL, d, n, o, NULL); }
int sb_stat(struct sb_client *c, const char *p, uint64_t *size)
{ return size ? request(c, SB_STAT, p, NULL, 0, 0, size) : -EINVAL; }
int sb_replace(struct sb_client *c, const char *p, const void *d, uint32_t n)
{ return request(c, SB_REPLACE, p, (void *)d, n, 0, NULL); }
int sb_unlink(struct sb_client *c, const char *p) { return request(c, SB_UNLINK, p, NULL, 0, 0, NULL); }
int sb_mkdir(struct sb_client *c, const char *p) { return request(c, SB_MKDIR, p, NULL, 0, 0, NULL); }
int sb_append(struct sb_client *c, const char *p, const void *d, uint32_t n)
{ return request(c, SB_APPEND, p, (void *)d, n, 0, NULL); }
int sb_truncate(struct sb_client *c, const char *p, uint64_t size)
{ return request(c, SB_TRUNCATE, p, NULL, 0, size, NULL); }
int sb_fsync(struct sb_client *c, const char *p)
{ return request(c, SB_FSYNC, p, NULL, 0, 0, NULL); }
int sb_rename(struct sb_client *c, const char *from, const char *to)
{
    size_t n;
    if (!to || !(n = strnlen(to, SB_MAX_PATH + 1u)) || n > SB_MAX_PATH) return -EINVAL;
    return request(c, SB_RENAME, from, (void *)to, (uint32_t)n, 0, NULL);
}
int sb_rmdir(struct sb_client *c, const char *p)
{ return request(c, SB_RMDIR, p, NULL, 0, 0, NULL); }
int sb_list(struct sb_client *c, const char *p, void *buffer, uint32_t capacity)
{ return request(c, SB_LIST, p, buffer, capacity, 0, NULL); }
int sb_repair(struct sb_client *c, struct sb_repair_stats *stats)
{
    if (!stats) return -EINVAL;
    memset(stats, 0, sizeof(*stats));
    return request(c, SB_REPAIR, NULL, stats, 0, 0, NULL);
}
int sb_install_home_art(struct sb_client *c, enum sb_home_art_kind kind, const void *data, uint32_t length)
{
    return request(c, SB_INSTALL_HOME_ART, NULL, (void *)data, length, (uint64_t)kind, NULL);
}
int sb_remove_home_backgrounds(struct sb_client *c)
{
    return request(c, SB_REMOVE_HOME_BACKGROUNDS, NULL, NULL, 0, 0, NULL);
}
int sb_quit(struct sb_client *c) { return request(c, SB_QUIT, NULL, NULL, 0, 0, NULL); }
