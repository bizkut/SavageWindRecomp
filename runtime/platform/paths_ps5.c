// PS5 data-root setup; title file reads use absolute paths, not the cwd.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../paths.h"
#include "../../ps5/storagebroker/client.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
static void fail_title(void) {
    fflush(stderr);
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) sleep(1);
}


const char *rt_data_root(void) { return SB_ROOT; }
bool rt_dev_mode(void) { return false; }

void rt_paths_init(void) {
    struct sb_client broker = {-1, 0};
    struct sb_repair_stats stats = {0};
    int repair = sb_connect(&broker);
    if (!repair) repair = sb_repair(&broker, &stats);
    sb_close(&broker);
    fprintf(stderr, "[paths] permission repair: result=%d scanned=%u changed=%u skipped=%u failed=%u\n",
            repair, stats.scanned, stats.changed, stats.skipped, stats.failed);
    if (repair < 0) {
        fprintf(stderr, "[paths] storage broker unavailable: %d\n", -repair);
        fail_title();
    }
    struct stat st = {0};
    errno = 0;
    int stat_result = stat(SB_ROOT, &st);
    int stat_errno = stat_result ? errno : 0;
    errno = 0;
    int chdir_result = chdir(SB_ROOT);
    int chdir_errno = chdir_result ? errno : 0;
    fprintf(stderr, "[paths] root=%s stat=%d errno=%d is_dir=%d broker_write=1 chdir=%d errno=%d\n",
            SB_ROOT, stat_result, stat_errno, !stat_result && S_ISDIR(st.st_mode), chdir_result, chdir_errno);
    if (stat_result || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "[paths] data root is inaccessible; load ShadowMountPlus and check %s\n", SB_ROOT);
        fail_title();
    }
    fprintf(stderr, "[paths] data: %s (absolute reads, broker writes)\n", SB_ROOT);
}

bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n) {
    (void)dev_rel;
    if (!name || !out || !n) return false;
    int count = snprintf(out, n, "/app0/%s", name);
    if (count < 0 || (size_t)count >= n) return false;
    struct stat st;
    return stat(out, &st) == 0;
}

// The player's own start-up picture (runtime/ui/splash.mm on desktop; the start-up splash in
// runtime/nvn/vulkan/device.c here): splash.jpg / splash.jpeg / splash.png in the data root. Never shipped.
bool rt_user_splash(char *out, size_t n) {
    static const char *const names[] = {"splash.jpg", "splash.jpeg", "splash.png"};
    struct stat st;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (rt_data_path(names[i], out, n) && stat(out, &st) == 0 && S_ISREG(st.st_mode)) return true;
    return false;
}
