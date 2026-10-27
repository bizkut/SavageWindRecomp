/*
 * Savage Wind PS5 storage-broker title benchmark.
 * Same workload as ps5/probe/io_probe.c, but every file operation crosses the loopback broker protocol. The client
 * is a normal title process; the broker is a root payload on port 9142. Output: /app0/storagebroker-bench.txt.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "client.h"
#include <pthread.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MIB (1024u * 1024u)
#define FILE_MB 3072
#define FILE_PATH "storage-broker-bench.bin"
#define SMALL_PATH "storage-broker-small.bin"

int sceSystemServiceLoadExec(const char *path, const char *const *argv);
static FILE *log_file;
static int failures;

static void say(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    printf("sbroker-bench: %s\n", line);
    fflush(stdout);
    if (log_file) fprintf(log_file, "%s\n", line), fflush(log_file);
}
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static uint64_t next_random(uint64_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}
static int connect_client(struct sb_client *client) {
    *client = (struct sb_client){-1, 0};
    int rc = sb_connect(client);
    if (rc) say("connect failed: %d", rc), failures++;
    return rc;
}
static double reads(struct sb_client *client, uint32_t block, unsigned count, int random_offsets, uint64_t seed) {
    unsigned char *data = aligned_alloc(64 * 1024, (block + 64 * 1024 - 1) & ~(64 * 1024 - 1));
    uint64_t offset = 0, bytes = 0;
    double start = now();
    if (!data) return -1;
    for (unsigned i = 0; i < count; i++) {
        if (random_offsets) offset = (next_random(&seed) % ((uint64_t)FILE_MB * MIB / block)) * block;
        int rc = sb_pread(client, FILE_PATH, data, block, offset);
        if (rc != (int)block) {
            say("read failed block=%u at=%llu rc=%d", block, (unsigned long long)offset, rc);
            failures++;
            break;
        }
        bytes += block;
        if (!random_offsets) offset += block;
    }
    double seconds = now() - start;
    free(data);
    return bytes / (double)MIB / seconds;
}
struct read_worker { uint64_t seed; double rate; int error; };
static void *read_thread(void *opaque) {
    struct read_worker *worker = opaque;
    struct sb_client client;
    if (connect_client(&client)) { worker->error = 1; return NULL; }
    worker->rate = reads(&client, 64 * 1024, 2000, 1, worker->seed);
    worker->error = worker->rate <= 0;
    sb_close(&client);
    return NULL;
}

static void expect_rejected(const char *name, int rc) {
    say("security %-28s rc=%d", name, rc);
    if (rc >= 0) failures++;
}

static void security_checks(struct sb_client *client) {
    static const char root[] = "/data/savagewind/";
    char path[256];
    uint64_t size;
    expect_rejected("parent traversal", sb_stat(client, "../homebrew/PPSA90212/eboot.bin", &size));
    expect_rejected("absolute path", sb_stat(client, "/data/homebrew/PPSA90212/eboot.bin", &size));
    expect_rejected("dot component", sb_stat(client, "save/./file", &size));
    expect_rejected("empty component", sb_stat(client, "save//file", &size));
    expect_rejected("broker token", sb_stat(client, ".storage-broker-token", &size));
    expect_rejected("broker temporary", sb_stat(client, "save/.storage-broker-tmp-x", &size));

    struct stat st = {0};
    snprintf(path, sizeof(path), "%ssecurity-final-link", root);
    if (!lstat(path, &st) && S_ISLNK(st.st_mode)) {
        expect_rejected("final symlink", sb_stat(client, "security-final-link", &size));
        unlink(path);
    } else {
        say("security final symlink fixture missing errno=%d", errno);
    }

    memset(&st, 0, sizeof(st));
    snprintf(path, sizeof(path), "%ssecurity-parent/link", root);
    if (!lstat(path, &st) && S_ISLNK(st.st_mode)) {
        expect_rejected("intermediate symlink", sb_stat(client, "security-parent/link/PPSA90212/eboot.bin", &size));
        unlink(path);
        rmdir("/data/savagewind/security-parent");
    } else {
        say("security intermediate symlink fixture missing errno=%d", errno);
    }

    memset(&st, 0, sizeof(st));
    snprintf(path, sizeof(path), "%ssecurity-hardlink", root);
    if (!lstat(path, &st) && S_ISREG(st.st_mode) && st.st_nlink > 1) {
        expect_rejected("multiply linked file", sb_stat(client, "security-hardlink", &size));
        unlink(path);
        unlink("/data/savagewind/security-original");
    } else {
        say("security hardlink fixture missing errno=%d nlink=%lu", errno, (unsigned long)st.st_nlink);
    }
}

int main(void) {
    struct sb_client client = {-1, 0};
    unsigned char *data = NULL;
    uint64_t size = 0;
    int rc, file_created = 0, small_created = 0;
    log_file = fopen("/app0/storagebroker-bench.txt", "w");
    say("Savage Wind PS5 storage broker benchmark");
    if (connect_client(&client)) goto done;
    security_checks(&client);
    rc = sb_create(&client, FILE_PATH);
    say("create: %d", rc);
    if (rc) { failures++; goto close; }
    file_created = 1;
    data = malloc(MIB);
    if (!data) { failures++; goto close; }
    for (unsigned i = 0; i < MIB; i++) data[i] = (unsigned char)(i * 31u);

    double total_start = now(), segment = total_start, worst = 0;
    for (unsigned m = 0; m < FILE_MB; m++) {
        double before = now();
        rc = sb_pwrite(&client, FILE_PATH, data, MIB, (uint64_t)m * MIB);
        double elapsed = now() - before;
        if (rc != (int)MIB) {
            say("W write %u MiB failed rc=%d", m, rc);
            failures++;
            goto close;
        }
        if (elapsed > worst) worst = elapsed;
        if ((m + 1) % 256 == 0) {
            say("W %4u MiB: %.0f MiB/s last 256 MiB, slowest request %.0f ms", m + 1,
                256.0 / (now() - segment), worst * 1e3);
            segment = now();
            worst = 0;
        }
    }
    say("W total %.0f MiB/s", FILE_MB / (now() - total_start));
    rc = sb_stat(&client, FILE_PATH, &size);
    say("stat: rc=%d size=%llu", rc, (unsigned long long)size);
    if (rc || size != (uint64_t)FILE_MB * MIB) failures++;

    double seq = reads(&client, MIB, 1024, 0, 0);
    double r64 = reads(&client, 64 * 1024, 4000, 1, 0x1234);
    double r4 = reads(&client, 4096, 8000, 1, 0x9876);
    say("R one connection: sequential 1 MiB %.0f MiB/s; random 64 KiB %.0f MiB/s (%.0f us/read); "
        "random 4 KiB %.1f MiB/s (%.0f us/read)", seq, r64, 64.0 / 1024 / r64 * 1e6,
        r4, 4.0 / 1024 / r4 * 1e6);

    struct read_worker workers[4];
    pthread_t threads[4];
    unsigned created = 0;
    double read_start = now();
    for (unsigned i = 0; i < 4; i++) {
        workers[i] = (struct read_worker){0x5555 + i, 0, 0};
        if (pthread_create(&threads[i], NULL, read_thread, &workers[i])) {
            failures++;
            break;
        }
        created++;
    }
    for (unsigned i = 0; i < created; i++) {
        pthread_join(threads[i], NULL);
        failures += workers[i].error;
    }
    say("R %u connections random 64 KiB: %.0f MiB/s", created,
        created * 2000.0 * 64 * 1024 / MIB / (now() - read_start));

    memset(data, 0xa5, 64 * 1024);
    double small_start = now();
    worst = 0;
    small_created = 1;
    for (unsigned i = 0; i < 200; i++) {
        double before = now();
        rc = sb_replace(&client, SMALL_PATH, data, 64 * 1024);
        double elapsed = now() - before;
        if (elapsed > worst) worst = elapsed;
        if (rc) { failures++; say("atomic replace %u failed rc=%d", i, rc); goto close; }
    }
    say("S 200 atomic 64 KiB replacements: %.2f ms each, slowest %.0f ms", (now() - small_start) / 200 * 1e3,
        worst * 1e3);
close:
    if (client.fd >= 0 && small_created) (void)sb_unlink(&client, SMALL_PATH);
    if (client.fd >= 0 && file_created) (void)sb_unlink(&client, FILE_PATH);
    free(data);
    sb_close(&client);
done:
    say("done: %d failed check(s)", failures);
    if (log_file) fclose(log_file);
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) usleep(100000);
}
