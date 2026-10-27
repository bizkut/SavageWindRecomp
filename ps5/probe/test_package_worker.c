// Host test for the dedicated PS5 package-open worker.
// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "../../runtime/gamelib.h"
#include "../../runtime/rt.h"
#include "../importer/package_exefs.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_t main_thread;
static int token, opens;
static uint8_t *(*reader)(const char *, size_t *, size_t);
static const uint8_t build_id[32] = {0xcd,0x57,0xb2,0x3f,0xa4,0xbb,0xad,0x65,0x80,0x3d,0x97,0x88,0xc0,0x18,0x21,0xee};
static const GameLibModule modules[] = {{.name = "main", .build_id = "cd57b23fa4bbad65803d9788c01821ee00000000000000000000000000000000"}};
static const uint32_t count = 1;
static const GameLibTables tables = {.modules = modules, .nmodules = &count};

static void check_worker(void) {
    assert(!pthread_equal(pthread_self(), main_thread));
    pthread_attr_t attr;
    assert(!pthread_getattr_np(pthread_self(), &attr));
    size_t size = 0;
    assert(!pthread_attr_getstacksize(&attr, &size));
    assert(size >= (8u << 20));
    pthread_attr_destroy(&attr);
    opens++;
}
uint64_t plat_now_ns(void) { return 0; }
const GameLibTables *gamelib_tables(void) { return &tables; }
void loader_set_file_reader(uint8_t *(*f)(const char *, size_t *, size_t)) { reader = f; }
void *savagewind_package_open(const char *keys, const char *const *p, size_t n, unsigned addon, char *why, size_t cap) {
    (void)keys; (void)p; (void)n; (void)addon; (void)why; (void)cap; check_worker(); return &token;
}
uint64_t savagewind_package_size(void *p) { assert(p == &token); return 42; }
void savagewind_package_close(void *p) { assert(p == &token); }
void *savagewind_exefs_open(const char *keys, const char *const *p, size_t n, char *why, size_t cap) {
    (void)keys; (void)p; (void)n; (void)why; (void)cap; check_worker(); return &token;
}
void *savagewind_exefs_file_open(void *p, const char *name) {
    assert(p == &token && !strcmp(name, "main")); return &token;
}
int64_t savagewind_exefs_file_size(void *p) { assert(p == &token); return 0x60; }
ssize_t savagewind_exefs_file_pread(void *p, void *dst, size_t size, uint64_t off) {
    assert(p == &token && size == 0x60 && off == 0);
    memset(dst, 0, size); memcpy(dst, "NSO0", 4); memcpy((char *)dst + 0x40, build_id, sizeof build_id); return size;
}
void savagewind_exefs_file_close(void *p) { assert(p == &token); }
void savagewind_exefs_close(void *p) { assert(p == &token); }
extern bool ps5_exefs_init(const char *, char *, size_t);
int main(void) {
    main_thread = pthread_self();
    char dir[] = "/tmp/savagewind-worker-XXXXXX", why[256] = "";
    assert(mkdtemp(dir));
    assert(ps5_exefs_init(dir, why, sizeof why));
    assert(opens == 2 && reader);
    size_t size = 0;
    uint8_t *bytes = reader("exefs/main", &size, 0x100);
    assert(bytes && size == 0x60 && !memcmp(bytes, "NSO0", 4));
    free(bytes);
    assert(!rmdir(dir));
    return 0;
}
