/* Host behavior/security test for the Savage Wind storage broker. GPL-3.0-or-later. */
#include "client.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static void check(int condition, const char *what) {
    printf("%s: %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) failures++;
}
int main(void) {
    struct sb_client client = {-1, 0};
    unsigned char in[64 * 1024], out[sizeof(in)];
    uint64_t size = 0;
    for (size_t i = 0; i < sizeof(in); i++) in[i] = (unsigned char)(i * 31u);
    check(sb_connect(&client) == 0, "connect/version/token");
    if (client.fd < 0) return 1;
    check(sb_mkdir(&client, "save") == 0, "mkdir");
    check(sb_create(&client, "save/file") == 0, "create");
    check(sb_pwrite(&client, "save/file", in, sizeof(in), 0) == (int)sizeof(in), "pwrite");
    check(sb_stat(&client, "save/file", &size) == 0 && size == sizeof(in), "stat size");
    memset(out, 0, sizeof(out));
    check(sb_pread(&client, "save/file", out, sizeof(out), 0) == (int)sizeof(out) && !memcmp(in, out, sizeof(in)),
          "pread bytes");
    memset(in, 0xa5, 12345);
    check(sb_replace(&client, "save/file", in, 12345) == 0, "atomic replace");
    check(sb_stat(&client, "save/file", &size) == 0 && size == 12345, "replace size");
    check(sb_unlink(&client, "save/file") == 0, "unlink regular");

    check(sb_stat(&client, "../escape", &size) < 0, "reject parent traversal");
    check(sb_stat(&client, "/absolute", &size) < 0, "reject absolute path");
    check(sb_stat(&client, "save/./file", &size) < 0, "reject dot component");
    check(sb_stat(&client, "save//file", &size) < 0, "reject empty component");
    check(sb_stat(&client, SB_TOKEN_NAME, &size) < 0, "reject broker token");

    char path[512];
    snprintf(path, sizeof(path), "%s/final-link", SB_ROOT);
    unlink(path);
    check(symlink("/etc/passwd", path) == 0, "make final symlink fixture");
    check(sb_stat(&client, "final-link", &size) < 0, "reject final symlink");
    unlink(path);

    snprintf(path, sizeof(path), "%s/intermediate", SB_ROOT);
    mkdir(path, 0777);
    char link_path[560];
    snprintf(link_path, sizeof(link_path), "%s/link", path);
    unlink(link_path);
    check(symlink("/etc", link_path) == 0, "make intermediate symlink fixture");
    check(sb_stat(&client, "intermediate/link/passwd", &size) < 0, "reject intermediate symlink");
    unlink(link_path);
    rmdir(path);

    char original[512], hard[512];
    snprintf(original, sizeof(original), "%s/original", SB_ROOT);
    snprintf(hard, sizeof(hard), "%s/hard", SB_ROOT);
    unlink(original); unlink(hard);
    int fd = open(original, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        if (write(fd, "x", 1) != 1) failures++;
        close(fd);
    }
    check(fd >= 0 && link(original, hard) == 0, "make hard-link fixture");
    check(sb_stat(&client, "hard", &size) < 0, "reject multiply linked file");
    unlink(hard); unlink(original);

    sb_close(&client);
    rmdir(SB_ROOT "/save");
    printf("done: %d failed check(s)\n", failures);
    return failures != 0;
}
