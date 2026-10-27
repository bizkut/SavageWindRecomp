// Standalone broker-backed runtime storage smoke; no game assets required.
#include "../../runtime/storage_ps5.h"
#include "../storagebroker/client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#ifdef __PROSPERO__
#include <ps5platform/klog.h>
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
#endif

static void say(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
#ifdef __PROSPERO__
    sceKernelDebugOutText(0, line);
#else
    fputs(line, stderr);
#endif
}

static int finish(int status) {
    say("storage runtime: finished status=%d\n", status);
#ifdef __PROSPERO__
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) sleep(1);
#else
    return status;
#endif
}

static int repair_smoke(void) {
    static const char *paths[] = {
        SB_ROOT "/permission-smoke", SB_ROOT "/permission-smoke/nested",
        SB_ROOT "/permission-smoke/nested/file"
    };
    for (size_t i = 0; i < 3; i++) {
        struct stat st;
        if (stat(paths[i], &st) == 0) say("before %s mode=%04o\n", paths[i], (unsigned)(st.st_mode & 07777));
        else say("before %s inaccessible errno=%d\n", paths[i], errno);
    }
    struct sb_client client = {-1, 0};
    struct sb_repair_stats stats = {0};
    int r = sb_connect(&client);
    if (r == 0) r = sb_repair(&client, &stats);
    sb_close(&client);
    say("repair result=%d scanned=%u changed=%u skipped=%u failed=%u\n",
        r, stats.scanned, stats.changed, stats.skipped, stats.failed);
    if (r != 0 || stats.failed || stats.changed < 3) return 1;
    for (size_t i = 0; i < 3; i++) {
        struct stat st;
        if (stat(paths[i], &st)) { say("after %s: errno=%d\n", paths[i], errno); return 1; }
        say("after %s mode=%04o\n", paths[i], (unsigned)(st.st_mode & 07777));
        if ((st.st_mode & 07777) != (i == 2 ? 0666 : 0777)) return 1;
    }
    return 0;
}

#define CHECK(x) do { if (!(x)) { say("storage smoke failed: %s (line %d), errno=%d\n", #x, __LINE__, errno); return finish(1); } } while (0)
int main(void) {
    say("storage runtime: entered main\n");
    const char *dir = "storage-runtime-smoke";
    CHECK(repair_smoke() == 0);
    const char *file = "storage-runtime-smoke/save";
    const char *renamed = "storage-runtime-smoke/save-renamed";
    const char *settings = "storage-runtime-smoke/settings.ini";
    const char *cache = "storage-runtime-smoke/cache.bin";
    const char text[] = "fullscreen = 1\nvolume = 0.75\n";
    unsigned char check[32];
    CHECK(rt_storage_mkdirs(dir) == 0);
    CHECK(rt_storage_create(file) == 0);
    CHECK(rt_storage_write(file, "hello", 5, 9) == 5);
    CHECK(rt_storage_write(file, "abc", 3, 1) == 3);
    CHECK(rt_storage_truncate(file, 12) == 0);
    CHECK(rt_storage_fsync(file) == 0);
    CHECK(rt_storage_read(file, check, 12, 0) == 12);
    CHECK(!memcmp(check + 1, "abc", 3) && !memcmp(check + 9, "hel", 3));
    say("storage runtime: initial save operations passed\n");
    CHECK(rt_storage_truncate(file, 0) == 0);
    CHECK(rt_storage_truncate(file, 256) == 0);
    unsigned char expected[256] = {0}, observed[256];
    uint32_t seed = 0x12345678;
    for (int i = 0; i < 128; i++) {
        seed = seed * 1664525u + 1013904223u;
        unsigned off = seed & 255u;
        unsigned char value = (unsigned char)(seed >> 24);
        CHECK(rt_storage_write(file, &value, 1, off) == 1);
        expected[off] = value;
    }
    CHECK(rt_storage_fsync(file) == 0);
    CHECK(rt_storage_read(file, observed, sizeof observed, 0) == sizeof observed);
    CHECK(!memcmp(expected, observed, sizeof expected));
    say("storage runtime: random save operations passed\n");
    CHECK(rt_storage_rename(file, renamed) == 0);
    CHECK(rt_storage_unlink(renamed) == 0);
    CHECK(rt_storage_replace(settings, text, sizeof text) == 0);
    CHECK(rt_storage_read(settings, check, sizeof text, 0) == sizeof text);
    CHECK(!memcmp(check, text, sizeof text));
    say("storage runtime: settings replacement passed\n");
    size_t n = 16u * 1024u * 1024u + 8193u;
    unsigned char *data = malloc(n), *out = malloc(n);
    CHECK(data && out);
    for (size_t i = 0; i < n; i++) data[i] = (unsigned char)(i * 37u + (i >> 12));
    CHECK(rt_storage_replace(cache, data, n) == 0);
    CHECK(rt_storage_read(cache, out, n, 0) == (ssize_t)n);
    CHECK(!memcmp(data, out, n));
    free(data); free(out);
    CHECK(rt_storage_unlink(settings) == 0);
    CHECK(rt_storage_unlink(cache) == 0);
    CHECK(rt_storage_rmdir(dir) == 0);
    say("storage runtime smoke passed\n");
    return finish(0);
}
