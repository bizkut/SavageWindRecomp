// Exercise save-file and directory service lifetimes beyond a small descriptor limit.
// clang -D_GNU_SOURCE -std=gnu11 -ffunction-sections -fdata-sections \
//   -Ibuild/ps5/host-deps/root/usr/include -include SDL3/SDL.h \
//   tools/test_fs_lifetime.c -Wl,--gc-sections -o /tmp/test_fs_lifetime && /tmp/test_fs_lifetime
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>

#include "../runtime/platform/linux_compat.h"
#include "../runtime/services/fs.c"
#include "../runtime/ipc.c"

const char *rt_data_root(void) { return ""; }
bool test_warp_active(void) { return false; }
void test_warp_patch(uint8_t *data, uint64_t size) { (void)data; (void)size; }
void kernel_watch_write(const char *what, uint64_t addr, uint64_t len) {
    (void)what;
    (void)addr;
    (void)len;
}

static unsigned count_fds(void) {
    unsigned count = 0;
    for (int fd = 0; fd < 64; fd++) if (fcntl(fd, F_GETFD) >= 0) count++;
    return count;
}

int main(void) {
    char root[] = "/tmp/savagewind-fs-lifetime-XXXXXX";
    assert(mkdtemp(root));
    char path[1024];
    snprintf(path, sizeof path, "%s/picture.jpg", root);
    int fd = open(path, O_CREAT | O_WRONLY, 0600);
    assert(fd >= 0);
    assert(close(fd) == 0);
    struct rlimit limit;
    assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    assert(limit.rlim_cur >= 64);
    limit.rlim_cur = 64;
    assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    unsigned before = count_fds();
    HostFs fs = {0};
    snprintf(fs.root, sizeof fs.root, "%s", root);
    Service parent = {.state = &fs};
    uint32_t mode = 1;
    for (int i = 0; i < 1024; i++) {
        const char *name = i & 1 ? "/" : "/picture.jpg";
        IpcReq req = {.cmd = i & 1 ? 9 : 8, .data = (const uint8_t *)&mode, .nx = 1};
        req.x[0].addr = (uintptr_t)name;
        IpcResp resp = {0};
        filesystem(&parent, &req, &resp);
        assert(resp.result == 0 && resp.nobjects == 1);
        Service *opened = resp.objects[0];
        assert(opened->on_close);
        opened->refs = 2;
        service_release(opened);
        assert(opened->state);
        service_release(opened);
        free((void *)opened->name);
        free(opened);
    }
    assert(count_fds() == before);
    assert(unlink(path) == 0);
    assert(rmdir(root) == 0);
    return 0;
}
