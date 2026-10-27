// Host test: title data reads do not depend on successful chdir.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/paths.h"
#include "../storagebroker/client.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int missing_root;
int sb_connect(struct sb_client *c) { c->fd = 1; return 0; }
int sb_repair(struct sb_client *c, struct sb_repair_stats *s) { (void)c; memset(s, 0, sizeof *s); return 0; }
int sceSystemServiceLoadExec(const char *path, const char *const *argv) {
    assert(!strcmp(path, "exit")); (void)argv; _exit(1);
}
void sb_close(struct sb_client *c) { c->fd = -1; }
int __wrap_stat(const char *path, struct stat *st) {
    assert(!strcmp(path, SB_ROOT));
    if (missing_root) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFDIR | 0755;
    return 0;
}
int __wrap_chdir(const char *path) { assert(!strcmp(path, SB_ROOT)); errno = EPERM; return -1; }

int main(void) {
    rt_paths_init(); // a title may not chdir, but the data root remains available
    char path[128];
    assert(rt_data_path("savagewind.ini", path, sizeof path));
    assert(!strcmp(path, SB_ROOT "/savagewind.ini"));
    assert(rt_data_path("save/save_0/file", path, sizeof path));
    assert(!strcmp(path, SB_ROOT "/save/save_0/file"));
    assert(rt_data_path("/data/other", path, sizeof path));
    assert(!strcmp(path, "/data/other"));
    assert(!rt_data_path("fonts/a", path, 4));
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) { missing_root = 1; rt_paths_init(); _exit(0); }
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    return 0;
}
