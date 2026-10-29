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
#include <dirent.h>
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
/* Keep path checks and namespace changes indivisible relative to other clients. */
static pthread_mutex_t request_lock = PTHREAD_MUTEX_INITIALIZER;
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
static int list_directory(int dir, unsigned char *data, uint32_t capacity, int root)
{
    DIR *stream = fdopendir(dir);
    struct dirent *entry;
    uint32_t used = 0;
    int rc = 0;
    if (!stream) { rc = -errno; close(dir); return rc; }
    for (;;) {
        struct stat st;
        struct sb_list_entry header;
        size_t n;
        errno = 0;
        entry = readdir(stream);
        if (!entry) { if (errno) rc = -errno; break; }
        n = strlen(entry->d_name);
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
            !strncmp(entry->d_name, TEMP_PREFIX, sizeof(TEMP_PREFIX) - 1) ||
            (root && !strcmp(entry->d_name, SB_TOKEN_NAME))) continue;
        if (fstatat(dir, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            if (errno == ENOENT) continue;
            rc = -errno; break;
        }
        if (S_ISDIR(st.st_mode)) header.type = SB_ENTRY_DIRECTORY;
        else if (S_ISREG(st.st_mode) && st.st_nlink == 1) header.type = SB_ENTRY_REGULAR;
        else continue;
        header.name_length = (uint32_t)n;
        if (sizeof(header) + n > capacity - used) { rc = -ENOSPC; break; }
        memcpy(data + used, &header, sizeof(header));
        used += sizeof(header);
        memcpy(data + used, entry->d_name, n);
        used += (uint32_t)n;
    }
    if (closedir(stream) && !rc) rc = -errno;
    return rc ? rc : (int)used;
}
struct repair_context {
    struct sb_repair_stats stats;
    dev_t device;
    int result;
    int limit_hit;
};
static void repair_failure(struct repair_context *ctx, int error)
{
    ctx->stats.failed++;
    if (!ctx->result) ctx->result = -error;
}
static void repair_directory(int fd, unsigned depth, struct repair_context *ctx);
static void repair_entry(int parent, const char *name, unsigned depth, struct repair_context *ctx)
{
    struct stat before, opened;
    int fd, flags;
    mode_t wanted;
    if (ctx->stats.scanned >= SB_REPAIR_MAX_ENTRIES || depth > SB_REPAIR_MAX_DEPTH) {
        repair_failure(ctx, E2BIG);
        ctx->limit_hit = 1;
        return;
    }
    ctx->stats.scanned++;
    if (fstatat(parent, name, &before, AT_SYMLINK_NOFOLLOW)) {
        repair_failure(ctx, errno); return;
    }
    if (S_ISLNK(before.st_mode)) { ctx->stats.skipped++; return; }
    if (before.st_dev != ctx->device) { repair_failure(ctx, EXDEV); return; }
    if (!S_ISREG(before.st_mode) && !S_ISDIR(before.st_mode)) {
        repair_failure(ctx, EINVAL); return;
    }
    if (S_ISREG(before.st_mode) && before.st_nlink != 1) {
        repair_failure(ctx, EMLINK); return;
    }
    wanted = S_ISDIR(before.st_mode) ? 0777 : 0666;
    if (S_ISREG(before.st_mode) && (before.st_mode & 07777) == wanted) return;
    flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
    if (S_ISDIR(before.st_mode)) flags |= O_DIRECTORY;
    fd = openat(parent, name, flags);
    if (fd < 0) { repair_failure(ctx, errno); return; }
    if (fstat(fd, &opened)) {
        repair_failure(ctx, errno); close(fd); return;
    }
    if (opened.st_dev != before.st_dev || opened.st_ino != before.st_ino ||
        (opened.st_mode & S_IFMT) != (before.st_mode & S_IFMT)) {
        repair_failure(ctx, ESTALE); close(fd); return;
    }
    if (S_ISREG(opened.st_mode) && opened.st_nlink != 1) {
        repair_failure(ctx, EMLINK); close(fd); return;
    }
    if ((opened.st_mode & 07777) != wanted) {
        if (fchmod(fd, wanted)) repair_failure(ctx, errno);
        else ctx->stats.changed++;
    }
    if (S_ISDIR(opened.st_mode)) repair_directory(fd, depth, ctx);
    close(fd);
}
static void repair_directory(int fd, unsigned depth, struct repair_context *ctx)
{
    int scan = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *stream;
    if (scan < 0) { repair_failure(ctx, errno); return; }
    stream = fdopendir(scan);
    if (!stream) { int e = errno; close(scan); repair_failure(ctx, e); return; }
    while (!ctx->limit_hit) {
        struct dirent *entry;
        errno = 0;
        entry = readdir(stream);
        if (!entry) { if (errno) repair_failure(ctx, errno); break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        repair_entry(fd, entry->d_name, depth + 1, ctx);
    }
    if (closedir(stream)) repair_failure(ctx, errno);
}
static int repair_root(struct sb_repair_stats *stats)
{
    struct repair_context ctx = {0};
    struct stat st;
    ctx.stats.scanned = 1;
    if (fstat(root_fd, &st)) repair_failure(&ctx, errno);
    else {
        ctx.device = st.st_dev;
        if ((st.st_mode & 07777) != 0777) {
            if (fchmod(root_fd, 0777)) repair_failure(&ctx, errno);
            else ctx.stats.changed++;
        }
        repair_directory(root_fd, 0, &ctx);
    }
    *stats = ctx.stats;
    return ctx.result;
}


static int do_request(struct sb_request *r, char *path, unsigned char *data, uint64_t *value)
{
    char *leaf = NULL, *dest_leaf = NULL;
    int dir, fd, dest_dir, rc = 0;
    struct stat st, dest_st;
    ssize_t count;
    if (r->operation == SB_LIST && r->path_length == 1 && path[0] == '.') {
        fd = openat(root_fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        return fd < 0 ? -errno : list_directory(fd, data, r->length, 1);
    }
    if (!path_valid(path, r->path_length)) return -EINVAL;
    if (r->operation == SB_RENAME && !path_valid((char *)data, r->length)) return -EINVAL;
    dir = parent_for(path, &leaf);
    if (dir < 0) return dir;
    switch (r->operation) {
    case SB_CREATE:
        fd = open_regular(dir, leaf, O_WRONLY | O_CREAT);
        if (fd < 0) { rc = fd; break; }
        if (ftruncate(fd, 0)) rc = -errno;
        close(fd);
        break;
    case SB_CREATE_EXCLUSIVE:
        fd = open_regular(dir, leaf, O_WRONLY | O_CREAT | O_EXCL);
        if (fd < 0) { rc = fd; break; }
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
    case SB_APPEND:
        fd = open_regular(dir, leaf, O_WRONLY | O_APPEND);
        if (fd < 0) { rc = fd; break; }
        do { count = write(fd, data, r->length); } while (count < 0 && errno == EINTR);
        rc = count < 0 ? -errno : (int)count;
        close(fd);
        break;
    case SB_TRUNCATE:
    case SB_FSYNC:
        fd = open_regular(dir, leaf, O_WRONLY);
        if (fd < 0) { rc = fd; break; }
        if (r->operation == SB_TRUNCATE ? ftruncate(fd, (off_t)r->offset) : fsync(fd))
            rc = -errno;
        close(fd);
        break;
    case SB_RENAME:
        dest_dir = parent_for((char *)data, &dest_leaf);
        if (dest_dir < 0) { rc = dest_dir; break; }
        if (fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW)) rc = -errno;
        else if (!S_ISDIR(st.st_mode) &&
                 !(S_ISREG(st.st_mode) && st.st_nlink == 1)) rc = -EINVAL;
        if (!rc && fstatat(dest_dir, dest_leaf, &dest_st, AT_SYMLINK_NOFOLLOW)) {
            if (errno != ENOENT) rc = -errno;
        } else if (!rc &&
                   !(S_ISDIR(st.st_mode) ? S_ISDIR(dest_st.st_mode) :
                     S_ISREG(dest_st.st_mode) && dest_st.st_nlink == 1)) rc = -EINVAL;
        if (!rc && renameat(dir, leaf, dest_dir, dest_leaf)) rc = -errno;
        if (!rc) {
            if (fsync(dir)) rc = -errno;
            if (fsync(dest_dir) && !rc) rc = -errno;
        }
        close(dest_dir);
        break;
    case SB_LIST:
        fd = openat(dir, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        rc = fd < 0 ? -errno : list_directory(fd, data, r->length, 0);
        break;
    case SB_REPLACE:
        rc = replace_file(dir, leaf, data, r->length);
        break;
    case SB_UNLINK:
        rc = existing_regular(dir, leaf, 0);
        if (!rc && unlinkat(dir, leaf, 0)) rc = -errno;
        if (!rc && fsync(dir)) rc = -errno;
        break;
    case SB_MKDIR:
        if (mkdirat(dir, leaf, 0777)) rc = -errno;
        else if (fsync(dir)) rc = -errno;
        break;
    case SB_RMDIR:
        if (unlinkat(dir, leaf, AT_REMOVEDIR)) rc = -errno;
        else if (fsync(dir)) rc = -errno;
        break;
    default: rc = -EINVAL; break;
    }
    close(dir);
    return rc;
}
static int valid_request(const struct sb_request *r)
{
    if (r->magic != SB_MAGIC || r->operation < SB_CREATE || r->operation > SB_PACKAGE_READ ||
        r->path_length > SB_MAX_PATH || r->length > SB_MAX_TRANSFER) return 0;
    if (r->operation == SB_QUIT || r->operation == SB_REPAIR)
        return !r->path_length && !r->length && !r->offset;
    if (r->operation == SB_PACKAGE_READ)
        return !r->path_length && r->offset <= INT64_MAX &&
               r->length <= (uint64_t)INT64_MAX - r->offset;
    if (!r->path_length) return 0;
    if (r->operation != SB_WRITE && r->operation != SB_READ &&
        r->operation != SB_TRUNCATE && r->offset) return 0;
    if (r->operation != SB_WRITE && r->operation != SB_READ &&
        r->operation != SB_REPLACE && r->operation != SB_APPEND &&
        r->operation != SB_RENAME && r->operation != SB_LIST && r->length) return 0;
    if (r->operation == SB_RENAME && (!r->length || r->length > SB_MAX_PATH)) return 0;
    if (r->operation == SB_TRUNCATE && r->offset > INT64_MAX) return 0;
    if ((r->operation == SB_WRITE || r->operation == SB_READ) &&
        (r->offset > INT64_MAX || r->length > (uint64_t)INT64_MAX - r->offset)) return 0;
    return 1;
}
static void *serve(void *arg)
{
    int package_fd = -1;
    int client = (int)(intptr_t)arg;
    struct sb_hello hello;
    struct sb_request r;
    char path[SB_MAX_PATH + 1];
    if (io_all(client, &hello, sizeof(hello), 0) || hello.magic != SB_MAGIC ||
        !hello.version || memcmp(hello.token, token, sizeof(token))) goto done;
    if (reply_to(client, hello.version == SB_VERSION ? 0 : -EPROTONOSUPPORT, 0)) goto done;
    while (!io_all(client, &r, sizeof(r), 0)) {
        unsigned char *data = NULL;
        struct sb_repair_stats repair_stats = {0};
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
        if (r.operation == SB_REPAIR) {
            pthread_mutex_lock(&request_lock);
            rc = repair_root(&repair_stats);
            pthread_mutex_unlock(&request_lock);
            if (reply_to(client, rc, 0) ||
                io_all(client, &repair_stats, sizeof(repair_stats), 1)) break;
            continue;
        }
        if (r.path_length && io_all(client, path, r.path_length, 0)) break;
        path[r.path_length] = 0;
        if (r.length && (r.operation == SB_READ || r.operation == SB_PACKAGE_READ || r.operation == SB_WRITE ||
                         r.operation == SB_REPLACE || r.operation == SB_APPEND ||
                         r.operation == SB_RENAME || r.operation == SB_LIST)) {
            data = malloc((size_t)r.length + (r.operation == SB_RENAME));
            if (!data) {
                unsigned char discard[4096];
                uint32_t remaining = (r.operation == SB_READ || r.operation == SB_PACKAGE_READ || r.operation == SB_LIST) ? 0 : r.length;
                while (remaining) {
                    size_t n = remaining < sizeof(discard) ? remaining : sizeof(discard);
                    if (io_all(client, discard, n, 0)) goto done;
                    remaining -= (uint32_t)n;
                }
                if (reply_to(client, -ENOMEM, 0)) break;
                continue;
            }
        }
        if ((r.operation == SB_WRITE || r.operation == SB_REPLACE ||
             r.operation == SB_APPEND || r.operation == SB_RENAME) && r.length &&
            io_all(client, data, r.length, 0)) { free(data); break; }
        if (r.operation == SB_RENAME) data[r.length] = 0;
        if (r.operation == SB_PACKAGE_OPEN) {
            char *leaf = NULL;
            int dir;
            if (package_fd >= 0) { close(package_fd); package_fd = -1; }
            if (r.path_length <= 7 || memcmp(path, "import/", 7) || !path_valid(path, r.path_length))
                rc = -EINVAL;
            else {
                pthread_mutex_lock(&request_lock);
                dir = parent_for(path, &leaf);
                if (dir < 0) rc = dir;
                else {
                    package_fd = open_regular(dir, leaf, O_RDONLY);
                    close(dir);
                    rc = package_fd < 0 ? package_fd : 0;
                    if (rc) package_fd = -1;
                }
                pthread_mutex_unlock(&request_lock);
            }
        } else if (r.operation == SB_PACKAGE_READ) {
            ssize_t count;
            if (package_fd < 0) rc = -EBADF;
            else {
                do { count = pread(package_fd, data, r.length, (off_t)r.offset); }
                while (count < 0 && errno == EINTR);
                rc = count < 0 ? -errno : (int)count;
            }
        } else {
            pthread_mutex_lock(&request_lock);
            rc = do_request(&r, path, data, &value);
            pthread_mutex_unlock(&request_lock);
        }
        if (reply_to(client, rc, value) ||
            ((r.operation == SB_READ || r.operation == SB_PACKAGE_READ || r.operation == SB_LIST) && rc > 0 &&
             io_all(client, data, (size_t)rc, 1))) {
            free(data); break;
        }
        free(data);
    }
done:
    if (package_fd >= 0) close(package_fd);
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
    if (data < 0) { say("open /data failed: errno=%d", errno); return -1; }
    if (mkdirat(data, "savagewind", 0777) && errno != EEXIST) {
        int error = errno;
        say("mkdirat savagewind failed: errno=%d", error);
        close(data); errno = error; return -1;
    }
    root = openat(data, "savagewind", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) {
        int error = errno;
        say("openat savagewind failed: errno=%d", error);
        close(data); errno = error; return -1;
    }
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
    {
        struct stat root_stat;
        if (fstat(root_fd, &root_stat) ||
            ((root_stat.st_mode & 07777) != 0777 && fchmod(root_fd, 0777))) {
            say("cannot make root accessible: errno=%d", errno); return 1;
        }
    }
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
    {
        struct sb_repair_stats stats;
        int rc = repair_root(&stats);
        say("startup repair result=%d scanned=%u changed=%u skipped=%u failed=%u",
            rc, stats.scanned, stats.changed, stats.skipped, stats.failed);
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
