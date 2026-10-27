// Host test: the M2 bootstrap boundary logs its requested name then exits.
// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#define main ps5_entry_unused
#include "../../runtime/platform/main_ps5.c"
#undef main
#include <assert.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int sink;
void ps5_logf(const char *format, ...) {
    char text[512]; va_list args;
    va_start(args, format); int n = vsnprintf(text, sizeof text, format, args); va_end(args);
    assert(n >= 0 && (size_t)n < sizeof text);
    assert(write(sink, text, (size_t)n) == n);
}
int sceSystemServiceLoadExec(const char *path, const char *const *argv) {
    assert(!strcmp(path, "exit") && !argv);
    assert(write(sink, "loadexec-exit\n", 14) == 14);
    _exit(0);
}
static void check(uint64_t name, const char *expected) {
    int fds[2]; assert(!pipe(fds));
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        close(fds[0]); sink = fds[1]; Cpu c = {0}; c.x[0] = name; first_nvn(&c); _exit(1);
    }
    close(fds[1]); char text[512]; size_t used = 0; ssize_t n;
    while (used < sizeof text - 1 && (n = read(fds[0], text + used, sizeof text - 1 - used)) > 0)
        used += (size_t)n;
    text[used] = 0; close(fds[0]); int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(strstr(text, expected) && strstr(text, "loadexec-exit\n"));
}
int main(void) {
    void *guest = mmap((void *)(uintptr_t)GUEST_LO, 0x4000, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(guest != MAP_FAILED);
    strcpy(guest, "nvnDeviceGetProcAddress");
    check(GUEST_LO, "nvnBootstrapLoader requested=nvnDeviceGetProcAddress");
    check(0, "nvnBootstrapLoader requested=<invalid>");
    assert(!munmap(guest, 0x4000));
    return 0;
}
