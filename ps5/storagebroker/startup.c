/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "startup.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef SB_BUNDLED_SHA256
#define SB_BUNDLED_SHA256 "unknown"
#endif

int sb_startup_with_ops(struct sb_client *client, int *loader_refused, const struct sb_startup_ops *ops)
{
    int rc;
    *loader_refused = 0;
    rc = ops->connect_broker(client);
    if (!rc && client->server_version >= SB_VERSION) return 0;
    ops->close_broker(client);
    rc = ops->launch();
    if (rc) {
        *loader_refused = rc == -ECONNREFUSED;
        return rc;
    }
    for (int i = 0; i < 100; i++) {
        rc = ops->connect_broker(client);
        if (!rc && client->server_version >= SB_VERSION) {
            fprintf(stderr, "[broker] started from the title (sha %s)\n", SB_BUNDLED_SHA256);
            return 0;
        }
        ops->close_broker(client);
        ops->pause();
    }
    return -ETIMEDOUT;
}

static int launch_bundled(void)
{
    struct sockaddr_in addr = {0};
    struct stat st;
    struct timespec start, now;
    char buffer[16384];
    int file = -1, sock = -1, rc = 0;
    file = open("/app0/storagebroker.elf", O_RDONLY);
    if (file < 0) return -errno;
    if (fstat(file, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) {
        rc = -EINVAL;
        goto done;
    }
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        rc = -errno;
        goto done;
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9021);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr))) {
        rc = -errno;
        goto done;
    }
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (off_t remaining = st.st_size; remaining > 0;) {
        size_t want = remaining < (off_t)sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        ssize_t n = read(file, buffer, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            rc = n ? -errno : -EIO;
            goto done;
        }
        remaining -= n;
        for (ssize_t sent = 0; sent < n;) {
            ssize_t count = send(sock, buffer + sent, (size_t)(n - sent), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (count > 0) {
                sent += count;
                continue;
            }
            if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                rc = -errno;
                goto done;
            }
            if (!count) {
                rc = -EPIPE;
                goto done;
            }
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - start.tv_sec >= 10) {
                rc = -ETIMEDOUT;
                goto done;
            }
            struct pollfd p = {sock, POLLOUT, 0};
            if (poll(&p, 1, 100) < 0 && errno != EINTR) {
                rc = -errno;
                goto done;
            }
        }
    }
    if (shutdown(sock, SHUT_WR)) rc = -errno;
done:
    if (sock >= 0) close(sock);
    close(file);
    return rc;
}

static void retry_pause(void)
{
    struct timespec delay = {0, 100000000};
    nanosleep(&delay, NULL);
}

int sb_startup(struct sb_client *client, int *loader_refused)
{
    static const struct sb_startup_ops ops = {sb_connect, sb_close, launch_bundled, retry_pause};
    int rc = sb_startup_with_ops(client, loader_refused, &ops);
    if (rc) fprintf(stderr, "[broker] startup failed: %d\n", -rc);
    return rc;
}
