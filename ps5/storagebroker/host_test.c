/* Host behavior/security test for the Savage Wind storage broker. GPL-3.0-or-later. */
#include "client.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static void check(int condition, const char *what) {
    printf("%s: %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) failures++;
}
static int list_has(const unsigned char *buffer, int length, const char *name, uint32_t type)
{
    size_t pos = 0, name_length = strlen(name);
    int found = 0;
    if (length < 0) return 0;
    while (pos < (size_t)length) {
        struct sb_list_entry entry;
        if ((size_t)length - pos < sizeof(entry)) return 0;
        memcpy(&entry, buffer + pos, sizeof(entry));
        pos += sizeof(entry);
        if (!entry.name_length || entry.name_length > (size_t)length - pos ||
            (entry.type != SB_ENTRY_REGULAR && entry.type != SB_ENTRY_DIRECTORY)) return 0;
        if (entry.type == type && entry.name_length == name_length &&
            !memcmp(buffer + pos, name, name_length)) found = 1;
        pos += entry.name_length;
    }
    return found;
}
struct append_worker { unsigned char byte; int result; };
static void *append_records(void *arg)
{
    struct append_worker *worker = arg;
    struct sb_client client = {-1, 0};
    unsigned char record[32];
    memset(record, worker->byte, sizeof(record));
    worker->result = sb_connect(&client);
    if (!worker->result) {
        for (int i = 0; i < 32; ++i) {
            if (sb_append(&client, "save/concurrent", record, sizeof(record)) != (int)sizeof(record)) {
                worker->result = -1;
                break;
            }
        }
    }
    sb_close(&client);
    return NULL;
}
static void test_home_art(struct sb_client *client)
{
    static const char *dirs[] = {
        SB_ROOT "/home-art", SB_ROOT "/home-art/user", SB_ROOT "/home-art/user/app",
        SB_ROOT "/home-art/user/app/PPSA90212", SB_ROOT "/home-art/user/app/PPSA90212/sce_sys",
        SB_ROOT "/home-art/user/appmeta", SB_ROOT "/home-art/user/appmeta/PPSA90212"
    };
    static const char *files[] = {
        SB_ROOT "/home-art/user/app/PPSA90212/sce_sys/icon0.png",
        SB_ROOT "/home-art/user/app/PPSA90212/icon0.png",
        SB_ROOT "/home-art/user/appmeta/PPSA90212/icon0.png",
        SB_ROOT "/home-art/user/app/PPSA90212/sce_sys/pic0.dds",
        SB_ROOT "/home-art/user/appmeta/PPSA90212/pic0.dds",
        SB_ROOT "/home-art/user/app/PPSA90212/sce_sys/pic1.dds",
        SB_ROOT "/home-art/user/appmeta/PPSA90212/pic1.dds"
    };
    static const unsigned char dds_header[148] = {
        [0] = 'D', [1] = 'D', [2] = 'S', [3] = ' ', [4] = 124,
        [8] = 0x07, [9] = 0x10, [10] = 0x0a,
        [12] = 0x70, [13] = 0x08, [17] = 0x0f,
        [21] = 0x90, [22] = 0x7e, [24] = 1, [28] = 1,
        [76] = 32, [80] = 4, [84] = 'D', [85] = 'X', [86] = '1', [87] = '0',
        [109] = 0x10, [128] = 98, [132] = 3, [140] = 1, [144] = 1
    };
    unsigned char png[33] = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13,
                             'I', 'H', 'D', 'R', 0, 0, 2, 0, 0, 0, 2, 0};
    unsigned char *dds = calloc(1, SB_HOME_ART_DDS_SIZE);
    struct stat before, after;
    int fd;
    int ok = dds != NULL;
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
        if (mkdir(dirs[i], 0700)) ok = 0;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        fd = open(files[i], O_WRONLY | O_CREAT | O_EXCL, 0640);
        if (fd < 0) ok = 0;
        else {
            if (write(fd, "old", 3) != 3) ok = 0;
            close(fd);
        }
    }
    if (dds) memcpy(dds, dds_header, sizeof(dds_header));
    check(ok, "home art fixtures");
    if (!ok) { free(dds); return; }
    check(stat(files[0], &before) == 0 &&
          sb_install_home_art(client, SB_HOME_ART_ICON0, png, sizeof(png)) == 0 &&
          stat(files[0], &after) == 0 && before.st_uid == after.st_uid && before.st_gid == after.st_gid &&
          (before.st_mode & 07777) == (after.st_mode & 07777), "install icon preserves metadata");
    for (size_t i = 0; i < 3; ++i) {
        unsigned char copy[sizeof(png)];
        fd = open(files[i], O_RDONLY);
        ok = fd >= 0 && read(fd, copy, sizeof(copy)) == sizeof(copy) && !memcmp(copy, png, sizeof(copy));
        if (fd >= 0) close(fd);
        check(ok, "icon fixed destination content");
    }
    for (size_t i = 3; i < 7; ++i)
        check(unlink(files[i]) == 0, "remove existing DDS fixture");
    check(sb_install_home_art(client, SB_HOME_ART_PIC0, dds, SB_HOME_ART_DDS_SIZE) == 0 &&
          sb_install_home_art(client, SB_HOME_ART_PIC1, dds, SB_HOME_ART_DDS_SIZE) == 0,
          "create missing DDS destinations for both kinds");
    for (size_t i = 3; i < 7; ++i) {
        unsigned char *copy = malloc(SB_HOME_ART_DDS_SIZE);
        fd = open(files[i], O_RDONLY);
        ok = copy && fd >= 0 && read(fd, copy, SB_HOME_ART_DDS_SIZE) == SB_HOME_ART_DDS_SIZE &&
             !memcmp(copy, dds, SB_HOME_ART_DDS_SIZE);
        if (fd >= 0) close(fd);
        free(copy);
        check(ok, "DDS fixed destination content");
    }
    check(stat(files[3], &before) == 0 && (before.st_mode & 07777) == 0644 &&
          chmod(files[3], 0600) == 0 && stat(files[3], &before) == 0 &&
          sb_install_home_art(client, SB_HOME_ART_PIC0, dds, SB_HOME_ART_DDS_SIZE) == 0 &&
          stat(files[3], &after) == 0 && before.st_uid == after.st_uid && before.st_gid == after.st_gid &&
          (after.st_mode & 07777) == 0600, "existing DDS metadata preserved on replacement");
    {
        struct sb_client old = {-1, 0};
        check(sb_connect_version(&old, 6) == 0 &&
              sb_install_home_art(&old, SB_HOME_ART_PIC0, dds, SB_HOME_ART_DDS_SIZE) == 0,
              "version 6 client retains home art operation");
        sb_close(&old);
    }
    {
        struct sb_client old = {-1, 0};
        check(sb_connect_version(&old, 7) == 0 &&
              sb_install_home_art(&old, SB_HOME_ART_PIC1, dds, SB_HOME_ART_DDS_SIZE) == 0,
              "version 7 client retains home art operation");
        sb_close(&old);
    }
    check(sb_install_home_art(client, (enum sb_home_art_kind)99, png, sizeof(png)) == -EINVAL &&
          sb_install_home_art(client, SB_HOME_ART_PIC0, dds, SB_HOME_ART_DDS_SIZE - 1) == -EINVAL &&
          sb_install_home_art(client, SB_HOME_ART_ICON0, png, 20) == -EINVAL, "reject kind and sizes");
    png[0] ^= 1;
    check(sb_install_home_art(client, SB_HOME_ART_ICON0, png, sizeof(png)) == -EINVAL, "reject PNG magic");
    png[0] ^= 1;
    png[18] ^= 1;
    check(sb_install_home_art(client, SB_HOME_ART_ICON0, png, sizeof(png)) == -EINVAL, "reject PNG dimensions");
    dds[0] ^= 1;
    check(sb_install_home_art(client, SB_HOME_ART_PIC0, dds, SB_HOME_ART_DDS_SIZE) == -EINVAL,
          "reject DDS magic");
    dds[0] ^= 1;
    dds[16] ^= 1;
    check(sb_install_home_art(client, SB_HOME_ART_PIC0, dds, SB_HOME_ART_DDS_SIZE) == -EINVAL,
          "reject DDS dimensions");
    dds[16] ^= 1;
    dds[128] ^= 1;
    check(sb_install_home_art(client, SB_HOME_ART_PIC0, dds, SB_HOME_ART_DDS_SIZE) == -EINVAL,
          "reject DDS format");
    dds[128] ^= 1;
    dds[28] = 2;
    check(sb_install_home_art(client, SB_HOME_ART_PIC0, dds, SB_HOME_ART_DDS_SIZE) == -EINVAL,
          "reject DDS mip count");
    dds[28] = 1;
    png[18] ^= 1;
    unlink(files[2]);
    check(stat(files[0], &before) == 0 &&
          sb_install_home_art(client, SB_HOME_ART_ICON0, png, sizeof(png)) < 0 &&
          stat(files[0], &after) == 0 && after.st_ino == before.st_ino,
          "failed preparation leaves destinations unchanged");
    check(symlink(files[0], files[2]) == 0 &&
          sb_install_home_art(client, SB_HOME_ART_ICON0, png, sizeof(png)) < 0 &&
          stat(files[0], &after) == 0 && after.st_ino == before.st_ino,
          "reject destination symlink without touching its target");
    unlink(files[2]);
    check(unlink(files[4]) == 0 && symlink(files[0], files[4]) == 0 &&
          sb_remove_home_backgrounds(client) < 0 && access(files[3], F_OK) == 0 &&
          stat(files[0], &after) == 0 && after.st_ino == before.st_ino,
          "background removal rejects symlink without touching files");
    unlink(files[4]);
    check(sb_remove_home_backgrounds(client) == 0 &&
          access(files[3], F_OK) < 0 && access(files[4], F_OK) < 0 &&
          access(files[5], F_OK) < 0 && access(files[6], F_OK) < 0 &&
          access(files[0], F_OK) == 0 && access(files[1], F_OK) == 0,
          "remove only fixed background files");
    check(sb_remove_home_backgrounds(client) == 0, "missing backgrounds are successful");
    free(dds);
}


int main(void) {
    struct sb_client client = {-1, 0};
    unsigned char in[64 * 1024], out[sizeof(in)];
    uint64_t size = 0;
    unsigned char listing[4096], small[1] = {0xa5};
    int listed;
    for (size_t i = 0; i < sizeof(in); i++) in[i] = (unsigned char)(i * 31u);
    check(sb_connect(&client) == 0, "connect/version/token");
    {
        struct sb_client old = {-1, 0};
        check(sb_connect_version(&old, 2) == -EPROTONOSUPPORT && old.fd == -1,
              "v8 rejects v2 requests");
        check(sb_connect_version(&old, 3) == -EPROTONOSUPPORT && old.fd == -1,
              "v8 rejects v3 requests");
        check(sb_connect_version(&old, 4) == -EPROTONOSUPPORT && old.fd == -1,
              "v8 rejects v4 requests");
        check(sb_connect_version(&old, 5) == 0 && sb_stat(&old, "missing", &size) == -ENOENT,
              "version 5 client retains existing operations");
        sb_close(&old);
    }
    if (client.fd < 0) return 1;
    test_home_art(&client);
    {
        int fd;
        check(sb_mkdir(&client, "import") == 0, "create import directory");
        fd = open(SB_ROOT "/import/game.nsp", O_CREAT | O_EXCL | O_WRONLY, 0600);
        check(fd >= 0 && write(fd, in, sizeof(in)) == (ssize_t)sizeof(in), "create package fixture");
        if (fd >= 0) close(fd);
        check(sb_package_read(&client, out, 8, 0) == -EBADF, "package read requires open");
        check(sb_package_open(&client, "save/file") == -EINVAL &&
              sb_package_open(&client, "import/../save/file") == -EINVAL,
              "package open confines path");
        check(symlink("/etc/passwd", SB_ROOT "/import/link") == 0 &&
              sb_package_open(&client, "import/link") < 0, "package open rejects symlink");
        unlink(SB_ROOT "/import/link");
        check(symlink("/etc", SB_ROOT "/import/parent") == 0 &&
              sb_package_open(&client, "import/parent/passwd") < 0,
              "package open rejects symlink parent");
        unlink(SB_ROOT "/import/parent");
        check(link(SB_ROOT "/import/game.nsp", SB_ROOT "/import/hard") == 0 &&
              sb_package_open(&client, "import/hard") < 0, "package open rejects hardlink");
        unlink(SB_ROOT "/import/hard");
        check(sb_package_open(&client, "import/game.nsp") == 0, "package open");
        check(sb_package_read(&client, out, sizeof(out), 0) == (int)sizeof(out) &&
              !memcmp(out, in, sizeof(in)), "package read returns file bytes");
        check(sb_package_read(&client, out, sizeof(out), sizeof(in) - 3) == 3 &&
              !memcmp(out, in + sizeof(in) - 3, 3), "package read short at EOF");
        check(sb_package_read(&client, out, 1, UINT64_MAX) == -EINVAL,
              "package read rejects overflowing offset");
        unlink(SB_ROOT "/import/game.nsp");
        rmdir(SB_ROOT "/import");
    }
    {
        struct sb_repair_stats stats;
        struct stat before = {0}, root_before = {0}, after, st;
        char outside[] = "/tmp/savagewind-repair-outside-XXXXXX";
        int outside_fd = mkstemp(outside);
        int file_fd;
        check(outside_fd >= 0, "create external repair target");
        if (outside_fd >= 0) close(outside_fd);
        check(chmod(outside, 0600) == 0, "set external target mode");
        check(mkdir(SB_ROOT "/repair", 0700) == 0 &&
              mkdir(SB_ROOT "/repair/nested", 0700) == 0, "create restrictive directories");
        file_fd = open(SB_ROOT "/repair/nested/file", O_CREAT | O_EXCL | O_WRONLY, 0600);
        check(file_fd >= 0, "create restrictive file");
        if (file_fd >= 0) close(file_fd);
        check(chmod(SB_ROOT "/repair/nested/file", 0400) == 0 &&
              stat(SB_ROOT "/repair/nested/file", &before) == 0 &&
              stat(SB_ROOT, &root_before) == 0 &&
              symlink(outside, SB_ROOT "/repair/escape") == 0 &&
              chmod(SB_ROOT, 0700) == 0, "prepare repair fixture");
        check(sb_repair(&client, &stats) == 0 && stats.failed == 0 &&
              stats.scanned >= 6 && stats.changed >= 4 && stats.skipped >= 1,
              "repair reports successful bounded traversal");
        check(stat(SB_ROOT, &st) == 0 && (st.st_mode & 07777) == 0777 &&
              st.st_uid == root_before.st_uid && st.st_gid == root_before.st_gid &&
              stat(SB_ROOT "/repair", &st) == 0 && (st.st_mode & 07777) == 0777 &&
              stat(SB_ROOT "/repair/nested", &st) == 0 && (st.st_mode & 07777) == 0777 &&
              stat(SB_ROOT "/repair/nested/file", &after) == 0 &&
              (after.st_mode & 07777) == 0666 && before.st_uid == after.st_uid &&
              before.st_gid == after.st_gid, "repair modes and preserve ownership");
        check(stat(outside, &st) == 0 && (st.st_mode & 07777) == 0600,
              "repair does not follow external symlink");
        check(mkfifo(SB_ROOT "/repair/fifo", 0600) == 0, "create special-file fixture");
        check(sb_repair(&client, &stats) == -EINVAL && stats.failed >= 1 &&
              stats.skipped >= 1, "repair rejects special files and returns counters");
        unlink(SB_ROOT "/repair/fifo");
        check(link(outside, SB_ROOT "/repair/hard") == 0, "create external hardlink fixture");
        check(sb_repair(&client, &stats) == -EMLINK && stats.failed >= 1 &&
              stat(outside, &st) == 0 && (st.st_mode & 07777) == 0600,
              "repair rejects hardlinks without changing external file");
        unlink(SB_ROOT "/repair/hard");
        {
            char deep[512] = SB_ROOT "/repair";
            size_t base = strlen(deep);
            int created = 0;
            for (int i = 0; i < 64; ++i) {
                size_t n = strlen(deep);
                if (n + 2 >= sizeof(deep)) break;
                memcpy(deep + n, "/d", 3);
                if (mkdir(deep, 0700)) { deep[n] = 0; break; }
                created++;
            }
            check(created == 64, "create deep repair fixture");
            check(sb_repair(&client, &stats) == -E2BIG && stats.failed >= 1,
                  "repair reports depth bound");
            while (strlen(deep) > base) {
                char *last = strrchr(deep, '/');
                rmdir(deep);
                *last = 0;
            }
        }
        unlink(SB_ROOT "/repair/escape");
        unlink(SB_ROOT "/repair/nested/file");
        rmdir(SB_ROOT "/repair/nested");
        rmdir(SB_ROOT "/repair");
        unlink(outside);
    }
    check(sb_mkdir(&client, "save") == 0, "mkdir");
    check(sb_create_exclusive(&client, "save/exclusive") == 0 &&
          sb_pwrite(&client, "save/exclusive", "keep", 4, 0) == 4,
          "exclusive create new file");
    check(sb_create_exclusive(&client, "save/exclusive") == -EEXIST &&
          sb_pread(&client, "save/exclusive", out, 4, 0) == 4 &&
          !memcmp(out, "keep", 4), "exclusive create preserves existing file");
    check(sb_create_exclusive(&client, "../escape") < 0 &&
          sb_unlink(&client, "save/exclusive") == 0, "exclusive create rejects traversal");
    check(sb_create(&client, "save/file") == 0, "create");
    check(sb_pwrite(&client, "save/file", in, sizeof(in), 0) == (int)sizeof(in), "pwrite");
    check(sb_stat(&client, "save/file", &size) == 0 && size == sizeof(in), "stat size");
    memset(out, 0, sizeof(out));
    check(sb_pread(&client, "save/file", out, sizeof(out), 0) == (int)sizeof(out) && !memcmp(in, out, sizeof(in)),
          "pread bytes");
    {
        struct append_worker workers[2] = {{'A', 0}, {'B', 0}};
        pthread_t threads[2];
        int started[2], valid = 1, counts[2] = {0, 0};
        check(sb_create(&client, "save/concurrent") == 0, "create concurrent append file");
        for (int i = 0; i < 2; ++i)
            started[i] = pthread_create(&threads[i], NULL, append_records, &workers[i]) == 0;
        for (int i = 0; i < 2; ++i) {
            if (started[i]) pthread_join(threads[i], NULL);
            if (!started[i] || workers[i].result) valid = 0;
        }
        if (sb_pread(&client, "save/concurrent", out, 2048, 0) != 2048) valid = 0;
        else for (int i = 0; i < 64; ++i) {
            unsigned char b = out[i * 32];
            if (b != 'A' && b != 'B') { valid = 0; continue; }
            counts[b == 'B']++;
            for (int j = 1; j < 32; ++j) if (out[i * 32 + j] != b) valid = 0;
        }
        check(valid && counts[0] == 32 && counts[1] == 32, "concurrent append records stay whole");
        check(sb_unlink(&client, "save/concurrent") == 0, "remove concurrent append file");
    }
    check(sb_append(&client, "save/file", "end", 3) == 3, "append bytes");
    check(sb_pread(&client, "save/file", out, 3, sizeof(in)) == 3 &&
          !memcmp(out, "end", 3), "append follows existing bytes");
    check(sb_truncate(&client, "save/file", 2) == 0, "truncate shorter");
    check(sb_stat(&client, "save/file", &size) == 0 && size == 2, "truncate size");
    check(sb_truncate(&client, "save/file", 5) == 0, "truncate extends");
    check(sb_pread(&client, "save/file", out, 5, 0) == 5 &&
          out[0] == in[0] && out[1] == in[1] &&
          out[2] == 0 && out[3] == 0 && out[4] == 0, "truncate zero fills");
    check(sb_truncate(&client, "save/file", UINT64_MAX) == -EINVAL,
          "reject unrepresentable truncate size");
    check(sb_fsync(&client, "save/file") == 0, "fsync regular file");
    check(sb_mkdir(&client, "cache") == 0, "mkdir rename destination");
    check(sb_rename(&client, "save/file", "cache/moved") == 0, "rename across directories");
    check(sb_stat(&client, "save/file", &size) == -ENOENT &&
          sb_stat(&client, "cache/moved", &size) == 0 && size == 5, "rename moved contents");
    check(sb_create(&client, "save/file") == 0, "create rename destination");
    check(sb_rename(&client, "cache/moved", "save/file") == 0, "rename replaces regular file");
    check(sb_rename(&client, "save/file", "cache") < 0, "rename rejects directory destination");
    check(sb_rename(&client, "cache", "save/file") < 0, "rename rejects file destination for directory");
    check(sb_mkdir(&client, "save/tree") == 0 &&
          sb_mkdir(&client, "save/tree/nested") == 0 &&
          sb_replace(&client, "save/tree/nested/leaf", "data", 4) == 0,
          "create directory rename tree");
    check(sb_rename(&client, "save/tree", "cache/moved") == 0,
          "rename populated directory across parents");
    check(sb_stat(&client, "cache/moved/nested/leaf", &size) == 0 && size == 4 &&
          sb_stat(&client, "save/tree/nested/leaf", &size) < 0,
          "directory rename preserves subtree");
    check(sb_rename(&client, "cache/moved", "cache/moved/nested/child") < 0,
          "rename directory into descendant rejected");
    check(sb_mkdir(&client, "save/empty") == 0 &&
          sb_rename(&client, "cache/moved", "save/empty") == 0,
          "rename directory replaces empty directory");
    check(sb_stat(&client, "save/empty/nested/leaf", &size) == 0 && size == 4,
          "replaced directory retains source subtree");
    check(sb_mkdir(&client, "cache/nonempty") == 0 &&
          sb_create(&client, "cache/nonempty/file") == 0,
          "create nonempty directory destination");
    check(sb_rename(&client, "save/empty", "cache/nonempty") < 0 &&
          sb_stat(&client, "save/empty/nested/leaf", &size) == 0 &&
          sb_stat(&client, "cache/nonempty/file", &size) == 0,
          "rename refuses nonempty directory replacement");
    check(sb_unlink(&client, "cache/nonempty/file") == 0 &&
          sb_rmdir(&client, "cache/nonempty") == 0 &&
          sb_unlink(&client, "save/empty/nested/leaf") == 0 &&
          sb_rmdir(&client, "save/empty/nested") == 0 &&
          sb_rmdir(&client, "save/empty") == 0, "remove directory rename fixtures");
    check(sb_list(&client, "cache", NULL, 0) == 0, "list empty directory");
    check(sb_rmdir(&client, "save") < 0, "rmdir rejects nonempty directory");
    listed = sb_list(&client, "save", listing, sizeof(listing));
    check(list_has(listing, listed, "file", SB_ENTRY_REGULAR), "list regular file");
    check(sb_list(&client, "save", small, sizeof(small)) == -ENOSPC && small[0] == 0xa5,
          "list insufficient capacity returns no partial data");
    check(sb_list(&client, "save", NULL, 0) == -ENOSPC, "list zero capacity");
    listed = sb_list(&client, ".", listing, sizeof(listing));
    check(list_has(listing, listed, "save", SB_ENTRY_DIRECTORY) &&
          list_has(listing, listed, "cache", SB_ENTRY_DIRECTORY), "list root directories");
    check(listed >= 0 && !list_has(listing, listed, SB_TOKEN_NAME, SB_ENTRY_REGULAR),
          "list hides token");
    {
        char temp[512];
        snprintf(temp, sizeof(temp), "%s/.storage-broker-tmp-fixture", SB_ROOT);
        int temp_fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);
        check(temp_fd >= 0, "create broker-owned name fixture");
        if (temp_fd >= 0) close(temp_fd);
        listed = sb_list(&client, ".", listing, sizeof(listing));
        check(listed >= 0 &&
              !list_has(listing, listed, ".storage-broker-tmp-fixture", SB_ENTRY_REGULAR),
              "list hides broker temporary names");
        check(sb_stat(&client, ".storage-broker-tmp-fixture", &size) < 0 &&
              sb_rename(&client, "save/file", ".storage-broker-tmp-fixture") < 0,
              "reject broker temporary names");
        unlink(temp);
    }
    check(sb_rmdir(&client, "cache") == 0, "rmdir empty directory");
    check(sb_list(&client, "save", listing, SB_MAX_TRANSFER + 1u) == -EINVAL,
          "list rejects oversized capacity");
    memset(in, 0xa5, 12345);
    check(sb_replace(&client, "save/file", in, 12345) == 0, "atomic replace");
    check(sb_stat(&client, "save/file", &size) == 0 && size == 12345, "replace size");
    check(sb_unlink(&client, "save/file") == 0, "unlink regular");

    check(sb_stat(&client, "../escape", &size) < 0, "reject parent traversal");
    check(sb_stat(&client, "/absolute", &size) < 0, "reject absolute path");
    check(sb_stat(&client, "save/./file", &size) < 0, "reject dot component");
    check(sb_stat(&client, "save//file", &size) < 0, "reject empty component");
    check(sb_rename(&client, "../escape", "save/file") < 0 &&
          sb_rename(&client, "save/file", "../escape") < 0, "rename rejects traversal in both paths");
    check(sb_list(&client, "../escape", listing, sizeof(listing)) < 0,
          "list rejects traversal");
    check(sb_rmdir(&client, "../escape") < 0, "rmdir rejects traversal");
    check(sb_stat(&client, SB_TOKEN_NAME, &size) < 0, "reject broker token");

    char path[512];
    snprintf(path, sizeof(path), "%s/final-link", SB_ROOT);
    unlink(path);
    check(symlink("/etc/passwd", path) == 0, "make final symlink fixture");
    check(sb_stat(&client, "final-link", &size) < 0, "reject final symlink");
    check(sb_create_exclusive(&client, "final-link") == -EEXIST,
          "exclusive create refuses symlink leaf");
    check(sb_append(&client, "final-link", "x", 1) < 0 &&
          sb_truncate(&client, "final-link", 0) < 0 &&
          sb_fsync(&client, "final-link") < 0, "file operations reject symlink");
    check(sb_rename(&client, "final-link", "save/moved") < 0 &&
          sb_rename(&client, "save/missing", "final-link") < 0,
          "rename rejects symlink source and destination");
    check(sb_create(&client, "save/source") == 0, "create security rename source");
    check(sb_rename(&client, "save/source", "../escape") == -EINVAL,
          "rename rejects traversal destination for existing source");
    check(sb_rename(&client, "save/source", "final-link") < 0,
          "rename refuses overwrite of symlink");
    check(sb_list(&client, "final-link", listing, sizeof(listing)) < 0 &&
          sb_rmdir(&client, "final-link") < 0, "directory operations reject symlink");
    listed = sb_list(&client, ".", listing, sizeof(listing));
    check(listed >= 0 && !list_has(listing, listed, "final-link", SB_ENTRY_REGULAR) &&
          !list_has(listing, listed, "final-link", SB_ENTRY_DIRECTORY), "list excludes symlink");
    unlink(path);

    snprintf(path, sizeof(path), "%s/intermediate", SB_ROOT);
    mkdir(path, 0777);
    char link_path[560];
    snprintf(link_path, sizeof(link_path), "%s/link", path);
    unlink(link_path);
    check(symlink("/etc", link_path) == 0, "make intermediate symlink fixture");
    check(sb_mkdir(&client, "save/dir-source") == 0, "create directory security source");
    check(sb_rename(&client, "save/dir-source", "intermediate/link") < 0 &&
          sb_rename(&client, "intermediate/link", "save/dir-target") < 0,
          "directory rename rejects symlink leaves");
    check(sb_rename(&client, "save/dir-source", "intermediate/link/moved") < 0 &&
          sb_rename(&client, "intermediate/link/source", "save/dir-target") < 0,
          "directory rename rejects symlink parents");
    check(sb_rename(&client, "save/dir-source", "../dir-target") == -EINVAL &&
          sb_rename(&client, "../dir-source", "save/dir-target") == -EINVAL,
          "directory rename rejects traversal");
    check(sb_rmdir(&client, "save/dir-source") == 0, "remove directory security source");
    check(sb_stat(&client, "intermediate/link/passwd", &size) < 0, "reject intermediate symlink");
    check(sb_rename(&client, "intermediate/link/passwd", "save/moved") < 0 &&
          sb_rename(&client, "save/source", "intermediate/link/moved") < 0,
          "rename rejects intermediate symlink in both paths");
    check(sb_list(&client, "intermediate/link", listing, sizeof(listing)) < 0,
          "list rejects intermediate symlink");
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
    check(sb_append(&client, "hard", "x", 1) < 0 &&
          sb_truncate(&client, "hard", 0) < 0 &&
          sb_fsync(&client, "hard") < 0, "file operations reject hardlink");
    check(sb_rename(&client, "hard", "save/moved") < 0 &&
          sb_rename(&client, "save/source", "hard") < 0,
          "rename rejects hardlink source and destination");
    listed = sb_list(&client, ".", listing, sizeof(listing));
    check(listed >= 0 && !list_has(listing, listed, "hard", SB_ENTRY_REGULAR) &&
          !list_has(listing, listed, "original", SB_ENTRY_REGULAR), "list excludes hardlinks");
    unlink(hard); unlink(original);
    check(sb_unlink(&client, "save/source") == 0, "remove rename source");
    check(sb_rmdir(&client, "save") == 0, "remove empty save directory");

    sb_close(&client);
    rmdir(SB_ROOT "/save");
    printf("done: %d failed check(s)\n", failures);
    return failures != 0;
}
