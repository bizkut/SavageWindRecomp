// Host test: runtime LOG and FATAL reach the klog sink before termination.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/rt.h"
#include <assert.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int sink;
int sceKernelDebugOutText(int channel, const char *text) {
    assert(channel == 0);
    size_t n = strlen(text);
    return write(sink, text, n) == (ssize_t)n ? 0 : -1;
}
void rt_fatal_trace(void) { _exit(42); }
int main(void) {
    int fds[2]; assert(!pipe(fds));
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(fds[0]); sink = fds[1];
        LOG("normal %d\n", 7);
        FATAL("missing service %s\n", "example");
    }
    close(fds[1]);
    char output[256]; size_t used = 0; ssize_t n;
    while (used < sizeof output - 1 && (n = read(fds[0], output + used, sizeof output - 1 - used)) > 0)
        used += (size_t)n;
    output[used] = 0; close(fds[0]);
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 42);
    assert(strstr(output, "savagewind: normal 7\n"));
    assert(strstr(output, "savagewind: FATAL: missing service example\n"));
    return 0;
}
