/* Root payload: restrictive fixtures for startup permission repair qualification. GPL-3.0-or-later. */
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    const char *dirs[] = {"/data/savagewind/permission-smoke", "/data/savagewind/permission-smoke/nested"};
    const char *files[] = {"/data/savagewind/permission-smoke/nested/file", "/data/savagewind-permission-smoke-outside"};
    int failed = 0;
    for (unsigned i = 0; i < 2; i++) {
        mkdir(dirs[i], 0700);
        if (chmod(dirs[i], 0700)) failed++;
    }
    for (unsigned i = 0; i < 2; i++) {
        int fd = open(files[i], O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd < 0) failed++;
        else {
            if (write(fd, "permission-test\n", 16) != 16 || fchmod(fd, 0600)) failed++;
            close(fd);
        }
    }
    printf("permission-setup: failed=%d\n", failed);
    return failed != 0;
}
