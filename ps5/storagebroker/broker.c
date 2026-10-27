/* SPDX-License-Identifier: GPL-3.0-or-later
 * Savage Wind root payload storage broker. The independent PS5 write broker
 * and ps5_writebroker.h are behavioral references; no code or protocol is
 * shared. The token is cooperative authentication, not a secret from local
 * homebrew: it is readable and writable by the title uid (mode 0666).
 * All filesystem requests are relative to one pre-opened directory, with
 * no-follow traversal; no operation grants access to arbitrary directories.
 */
#include "protocol.h"
#include "client.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __PROSPERO__
#include <ps5/klog.h>
#endif

static int root_fd;
static unsigned char token[SB_TOKEN_BYTES];
static pthread_mutex_t temp_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t temp_serial;
#define TEMP_PREFIX ".storage-broker-tmp-"

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
    char line[384];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
#ifdef __PROSPERO__
    klog_printf("savage-storage-broker: %s\n", line);
#else
    fprintf(stderr, "savage-storage-broker: %s\n", line);
#endif
}

static int io_all(int fd, void *buffer, size_t length, int writing)
{
    unsigned char *p = buffer;
    while (length) {
        ssize_t n = writing ? send(fd, p, length, MSG_NOSIGNAL) : recv(fd, p, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        length -= (size_t)n;
    }
    return 0;
}
static int reply_to(int fd, int result, uint64_t value)
{
    struct sb_reply r = { SB_MAGIC, result, SB_VERSION, 0, value };
    return io_all(fd, &r, sizeof(r), 1);
}

/* Reserve the credential at the root and temporary names in every directory,
 * so clients cannot interfere with broker-owned files. */
static int path_valid(const char *p, size_t n)
{
    size_t start = 0, i;
    if (!n || n > SB_MAX_PATH || p[0] == '/') return 0;
    for (i = 0; i <= n; ++i) {
        if (i < n && !p[i]) return 0;
        if (i == n || p[i] == '/') {
            size_t len = i - start;
            if (!len || (len == 1 && p[start] == '.') ||
                (len == 2 && p[start] == '.' && p[start + 1] == '.')) return 0;
            if ((!start && len == sizeof(SB_TOKEN_NAME) - 1 &&
                 !memcmp(p, SB_TOKEN_NAME, len)) ||
                (len >= sizeof(TEMP_PREFIX) - 1 &&
                 !memcmp(p + start, TEMP_PREFIX, sizeof(TEMP_PREFIX) - 1))) return 0;
            start = i + 1;
        }
    }
    return 1;
}
/* Returns a newly owned parent fd and a leaf inside the mutable path. */
static int parent_for(char *path, char **leaf)
{
    int current = dup(root_fd), next;
    char *part = path, *slash;
    if (current < 0) return -errno;
    while ((slash = strchr(part, '/'))) {
        *slash = 0;
        next = openat(current, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) { int e = -errno; close(current); return e; }
        close(current);
        current = next;
        part = slash + 1;
    }
    *leaf = part;
    return current;
}
static int open_regular(int dir, const char *leaf, int flags)
{
    struct stat st;
    int fd = openat(dir, leaf, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0666);
    if (fd < 0) return -errno;
    if (fstat(fd, &st)) { int e = -errno; close(fd); return e; }
    /* A multiply linked file could also name storage outside the root. */
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1) { close(fd); return -EINVAL; }
    return fd;
}
static int existing_regular(int dir, const char *leaf, int missing_ok)
{
    struct stat st;
    if (fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW))
        return errno == ENOENT && missing_ok ? 0 : -errno;
    return S_ISREG(st.st_mode) && st.st_nlink == 1 ? 0 : -EINVAL;
}
static int temp_file(int dir, char name[80])
{
    unsigned attempts;
    for (attempts = 0; attempts < 128; ++attempts) {
        uint64_t serial;
        int fd;
        pthread_mutex_lock(&temp_lock);
        serial = ++temp_serial;
        pthread_mutex_unlock(&temp_lock);
        snprintf(name, 80, TEMP_PREFIX "%ld-%llu", (long)getpid(), (unsigned long long)serial);
        fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -errno;
    }
    return -EEXIST;
}
static int write_all_file(int fd, const unsigned char *data, size_t n)
{
    while (n) {
        ssize_t written = write(fd, data, n);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return written ? -errno : -EIO;
        data += written;
        n -= (size_t)written;
    }
    return 0;
}
static int replace_file(int dir, const char *leaf, const unsigned char *data, size_t n)
{
    char name[80];
    int fd, rc = existing_regular(dir, leaf, 1);
    if (rc) return rc;
    fd = temp_file(dir, name);
    if (fd < 0) return fd;
    rc = write_all_file(fd, data, n);
    if (!rc && fsync(fd)) rc = -errno;
    if (close(fd) && !rc) rc = -errno;
    if (!rc) rc = existing_regular(dir, leaf, 1);
    if (!rc && renameat(dir, name, dir, leaf)) rc = -errno;
    if (!rc && fsync(dir)) rc = -errno;
    if (rc) unlinkat(dir, name, 0);
    return rc;
}
static int do_request(struct sb_request *r, char *path, unsigned char *data, uint64_t *value)
{
    char *leaf = NULL;
    int dir, fd, rc = 0;
    struct stat st;
    ssize_t count;
    if (!path_valid(path, r->path_length)) return -EINVAL;
    dir = parent_for(path, &leaf);
    if (dir < 0) return dir;
    switch (r->operation) {
    case SB_CREATE:
        fd = open_regular(dir, leaf, O_WRONLY | O_CREAT);
        if (fd < 0) { rc = fd; break; }
        if (ftruncate(fd, 0)) rc = -errno;
        close(fd);
        break;
    case SB_WRITE:
    case SB_READ:
        fd = open_regular(dir, leaf, r->operation == SB_WRITE ? O_WRONLY : O_RDONLY);
        if (fd < 0) { rc = fd; break; }
        do {
            count = r->operation == SB_WRITE ? pwrite(fd, data, r->length, (off_t)r->offset)
                                             : pread(fd, data, r->length, (off_t)r->offset);
        } while (count < 0 && errno == EINTR);
        rc = count < 0 ? -errno : (int)count;
        close(fd);
        break;
    case SB_STAT:
        fd = open_regular(dir, leaf, O_RDONLY);
        if (fd < 0) { rc = fd; break; }
        if (fstat(fd, &st)) rc = -errno;
        else *value = (uint64_t)st.st_size;
        close(fd);
        break;
    case SB_REPLACE:
        rc = replace_file(dir, leaf, data, r->length);
        break;
    case SB_UNLINK:
        rc = existing_regular(dir, leaf, 0);
        if (!rc && unlinkat(dir, leaf, 0)) rc = -errno;
        break;
    case SB_MKDIR:
        if (mkdirat(dir, leaf, 0777)) rc = -errno;
        break;
    default: rc = -EINVAL; break;
    }
    close(dir);
    return rc;
}
static int valid_request(const struct sb_request *r)
{
    if (r->magic != SB_MAGIC || r->operation < SB_CREATE || r->operation > SB_QUIT ||
        r->path_length > SB_MAX_PATH || r->length > SB_MAX_TRANSFER) return 0;
    if (r->operation == SB_QUIT) return !r->path_length && !r->length && !r->offset;
    if (!r->path_length) return 0;
    if (r->operation != SB_WRITE && r->operation != SB_READ && r->offset) return 0;
    if (r->operation != SB_WRITE && r->operation != SB_READ &&
        r->operation != SB_REPLACE && r->length) return 0;
    if ((r->operation == SB_WRITE || r->operation == SB_READ) &&
        (r->offset > INT64_MAX || r->length > (uint64_t)INT64_MAX - r->offset)) return 0;
    return 1;
}
static void *serve(void *arg)
{
    int client = (int)(intptr_t)arg;
    struct sb_hello hello;
    struct sb_request r;
    char path[SB_MAX_PATH + 1];
    if (io_all(client, &hello, sizeof(hello), 0) || hello.magic != SB_MAGIC ||
        !hello.version || memcmp(hello.token, token, sizeof(token))) goto done;
    if (reply_to(client, hello.version == SB_VERSION ? 0 : -EPROTONOSUPPORT, 0)) goto done;
    while (!io_all(client, &r, sizeof(r), 0)) {
        unsigned char *data = NULL;
        uint64_t value = 0;
        int rc;
        if (!valid_request(&r)) break;
        if (hello.version != SB_VERSION && r.operation != SB_QUIT) break;
        if (r.operation == SB_QUIT) {
            rc = hello.version > SB_VERSION ? 0 : -EPERM;
            if (reply_to(client, rc, 0)) break;
            if (!rc) exit(0);
            continue;
        }
        if (io_all(client, path, r.path_length, 0)) break;
        path[r.path_length] = 0;
        if (r.length && (r.operation == SB_READ || r.operation == SB_WRITE || r.operation == SB_REPLACE)) {
            data = malloc(r.length);
            if (!data) {
                unsigned char discard[4096];
                uint32_t remaining = r.operation == SB_READ ? 0 : r.length;
                while (remaining) {
                    size_t n = remaining < sizeof(discard) ? remaining : sizeof(discard);
                    if (io_all(client, discard, n, 0)) goto done;
                    remaining -= (uint32_t)n;
                }
                if (reply_to(client, -ENOMEM, 0)) break;
                continue;
            }
        }
        if ((r.operation == SB_WRITE || r.operation == SB_REPLACE) && r.length &&
            io_all(client, data, r.length, 0)) { free(data); break; }
        rc = do_request(&r, path, data, &value);
        if (reply_to(client, rc, value) ||
            (r.operation == SB_READ && rc > 0 && io_all(client, data, (size_t)rc, 1))) {
            free(data); break;
        }
        free(data);
    }
done:
    close(client);
    return NULL;
}
/* Establish the root without following either fixed ancestor. */
static int open_root(void)
{
#ifdef SB_HOST_TEST
    return open(SB_ROOT, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
#else
    int data = open("/data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC), root;
    if (data < 0) return -1;
    if (mkdirat(data, "savagewind", 0777) && errno != EEXIST) { close(data); return -1; }
    root = openat(data, "savagewind", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(data);
    return root;
#endif
}
static int publish_token(void)
{
    char name[80];
    size_t done = 0;
    int random = open("/dev/urandom", O_RDONLY | O_CLOEXEC), fd, rc;
    if (random < 0) return -1;
    while (done < sizeof(token)) {
        ssize_t n = read(random, token + done, sizeof(token) - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(random); return -1; }
        done += (size_t)n;
    }
    close(random);
    fd = temp_file(root_fd, name);
    if (fd < 0) return -1;
    rc = write_all_file(fd, token, sizeof(token));
    if (!rc && fchmod(fd, 0666)) rc = -errno;
    if (!rc && fsync(fd)) rc = -errno;
    if (close(fd) && !rc) rc = -errno;
    if (!rc) rc = existing_regular(root_fd, SB_TOKEN_NAME, 1);
    if (!rc && renameat(root_fd, name, root_fd, SB_TOKEN_NAME)) rc = -errno;
    if (rc) { unlinkat(root_fd, name, 0); return -1; }
    return 0;
}
int main(void)
{
    struct sockaddr_in address = {0};
    int listener, one = 1, attempt;
    umask(0);
    root_fd = open_root();
    if (root_fd < 0) { say("cannot open %s: errno=%d", SB_ROOT, errno); return 1; }
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { say("socket failed: errno=%d", errno); return 1; }
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) {
        say("SO_REUSEADDR failed: errno=%d", errno);
        return 1;
    }
    address.sin_family = AF_INET;
    address.sin_port = htons(SB_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (attempt = 0; bind(listener, (struct sockaddr *)&address, sizeof(address)); ++attempt) {
        struct sb_client old = { -1, 0 };
        if (errno != EADDRINUSE || attempt >= 20) return 1;
        if (!attempt && !sb_connect_version(&old, SB_VERSION)) {
            if (old.server_version < SB_VERSION) (void)sb_quit(&old);
            sb_close(&old);
        }
        usleep(100000);
    }
    if (listen(listener, 64) || publish_token()) {
        say("listen/token failed: errno=%d", errno);
        return 1;
    }
    say("listening on 127.0.0.1:%d for %s version=%u uid=%u euid=%u", SB_PORT, SB_ROOT, SB_VERSION,
        (unsigned)getuid(), (unsigned)geteuid());
    for (;;) {
        pthread_t thread;
        int client = accept(listener, NULL, NULL);
        if (client < 0) { if (errno == EINTR) continue; break; }
        if (setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) ||
            pthread_create(&thread, NULL, serve, (void *)(intptr_t)client)) {
            close(client); continue;
        }
        pthread_detach(thread);
    }
    close(listener);
    close(root_fd);
    return 1;
}
