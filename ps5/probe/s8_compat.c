// Missing title-libc interfaces used by LLVM; unsupported operations report failure.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <pwd.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

int mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    (void)name; (void)oldp; (void)oldlenp; (void)newp; (void)newlen;
    fprintf(stderr, "[s8] mallctl unavailable\n");
    return ENOTSUP;
}
int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t len, struct passwd **result) {
    (void)name; (void)pwd; (void)buf; (void)len;
    fprintf(stderr, "[s8] getpwnam_r unavailable\n");
    *result = NULL;
    return ENOTSUP;
}
int posix_madvise(void *addr, size_t len, int advice) {
    (void)addr; (void)len; (void)advice;
    fprintf(stderr, "[s8] posix_madvise unavailable\n");
    return ENOTSUP;
}
char *strsignal(int sig) {
    (void)sig;
    fprintf(stderr, "[s8] strsignal unavailable\n");
    return "signal description unavailable";
}
