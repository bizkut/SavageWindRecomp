/*
 * Savage Wind PS5 - storage probe for /data/savagewind (docs/PS5-PORT-PLAN.md "Data outside the title folder").
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The runtime reads romfs.bin with pread on one descriptor (runtime/romfs_image.c), from several threads, in sizes
 * from a few KiB to MiBs; it writes saves and shader caches. ps5-wow's slowness was a title write budget (~2400 MiB
 * fast, then down to ~0.5 MiB/s; skill ps5-title-write-broker), so this measures:
 *   W  sequential 1 MiB writes of a 3 GiB file, rate per 256 MiB (does the budget apply under /data?);
 *   R  reads of that file: sequential 1 MiB, random 64 KiB and 4 KiB, 4 threads random 64 KiB; buffered and O_DIRECT
 *      (O_DIRECT bypasses the cache the file just went through, as a first start's romfs reads would);
 *   S  small writes like saves/caches: 200 x create/write 64 KiB/close.
 * Output: /data/savagewind/io_probe.txt (and stdout). The 3 GiB file is deleted at the end.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __PROSPERO__
#define ROOT "/data/savagewind"
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
#else
#define ROOT "build/ps5/host/savagewind-data"
#endif
#ifndef O_DIRECT
#define O_DIRECT 0
#endif

#define MIB (1024ull * 1024)
#define FILE_MB 3072
static FILE *g_log;

static void say(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("io: %s\n", line);
    fflush(stdout);
    if (g_log) fprintf(g_log, "%s\n", line), fflush(g_log);
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static uint64_t rng(uint64_t *s) { *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17; return *s; }

static const char *g_path = ROOT "/io_probe.bin";

static double read_test(int flags, size_t block, int count, int random_offsets, uint64_t seed) {
    int fd = open(g_path, O_RDONLY | flags);
    if (fd < 0) return -errno;
    void *buf;
    if (posix_memalign(&buf, 64 * 1024, block)) return -1;
    double t0 = now();
    uint64_t bytes = 0, off = 0;
    for (int i = 0; i < count; i++) {
        if (random_offsets) off = (rng(&seed) % (FILE_MB * MIB / block)) * block;
        ssize_t r = pread(fd, buf, block, (off_t)off);
        if (r <= 0) break;
        bytes += (uint64_t)r;
        if (!random_offsets) off += block;
    }
    double t = now() - t0;
    free(buf);
    close(fd);
    return bytes / MIB / t;
}

struct job { int flags; size_t block; int count; uint64_t seed; double rate; };
static void *read_job(void *a) {
    struct job *j = a;
    j->rate = read_test(j->flags, j->block, j->count, 1, j->seed);
    return NULL;
}

static void reads(const char *what, int flags) {
    double seq = read_test(flags, MIB, 1024, 0, 0);
    double r64 = read_test(flags, 64 * 1024, 4000, 1, 0x1234);
    double r4 = read_test(flags, 4096, 8000, 1, 0x9876);
    struct job jobs[4];
    pthread_t th[4];
    double t0 = now();
    for (int i = 0; i < 4; i++) {
        jobs[i] = (struct job){flags, 64 * 1024, 2000, 0x5555 + i, 0};
        pthread_create(&th[i], NULL, read_job, &jobs[i]);
    }
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    double mt = 4 * 2000 * 64 * 1024 / MIB / (now() - t0);
    say("R %s: sequential 1 MiB %.0f MiB/s; random 64 KiB %.0f MiB/s (%.0f us/read); random 4 KiB %.1f MiB/s "
        "(%.0f us/read); 4 threads random 64 KiB %.0f MiB/s", what, seq, r64, r64 > 0 ? 64.0 / 1024 / r64 * 1e6 : 0,
        r4, r4 > 0 ? 4.0 / 1024 / r4 * 1e6 : 0, mt);
}

int main(void) {
    mkdir(ROOT, 0777);
    chmod(ROOT, 0777);
    g_log = fopen(ROOT "/io_probe.txt", "w");
    if (g_log) chmod(ROOT "/io_probe.txt", 0666);
    say("Savage Wind PS5 storage probe, %s (log %s)", ROOT, g_log ? "ok" : strerror(errno));

    /* W: 3 GiB sequential, per 256 MiB */
    int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    say("W open %s: fd=%d errno=%d", g_path, fd, fd < 0 ? errno : 0);
    if (fd >= 0) {
        uint8_t *buf = malloc(MIB);
        for (size_t i = 0; i < MIB; i++) buf[i] = (uint8_t)(i * 31);
        double t0 = now(), tseg = t0, worst = 0;
        for (int m = 0; m < FILE_MB; m++) {
            double tw = now();
            if (write(fd, buf, MIB) != (ssize_t)MIB) { say("W write %d MiB failed errno=%d", m, errno); break; }
            if (now() - tw > worst) worst = now() - tw;
            if ((m + 1) % 256 == 0) {
                say("W %4d MiB: %.0f MiB/s for the last 256 MiB, slowest 1 MiB write %.0f ms", m + 1,
                    256 / (now() - tseg), worst * 1e3);
                tseg = now(), worst = 0;
            }
        }
        double tf = now();
        fsync(fd);
        say("W total %.0f MiB/s, fsync %.0f ms", FILE_MB / (now() - t0), (now() - tf) * 1e3);
        close(fd);
        free(buf);
    }

    reads("buffered", 0);
    reads("O_DIRECT", O_DIRECT);

    /* S: save/cache-like small files */
    double t0 = now(), worst = 0;
    char name[128];
    uint8_t *small = calloc(1, 64 * 1024);
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof name, ROOT "/io_small_%03d.bin", i);
        double tw = now();
        int f = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (f < 0 || write(f, small, 64 * 1024) != 64 * 1024) { say("S file %d failed errno=%d", i, errno); break; }
        close(f);
        if (now() - tw > worst) worst = now() - tw;
    }
    say("S 200 x create/write 64 KiB/close: %.2f ms each, slowest %.0f ms", (now() - t0) / 200 * 1e3, worst * 1e3);
    for (int i = 0; i < 200; i++) snprintf(name, sizeof name, ROOT "/io_small_%03d.bin", i), unlink(name);
    free(small);
    unlink(g_path);
    say("done");
    if (g_log) fclose(g_log);
#ifdef __PROSPERO__
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) usleep(100000);
#endif
    return 0;
}
