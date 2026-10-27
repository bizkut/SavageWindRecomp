// Read the linked game's ExeFS directly from the user's packages.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gamelib.h"
#include "../game_versions.h"
#include "../rt.h"
#include "../../ps5/importer/package_exefs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
extern void *savagewind_package_open(const char *, const char *const *, size_t, unsigned, char *, size_t);
extern uint64_t savagewind_package_size(void *);
extern void savagewind_package_close(void *);


static void *exefs;

static uint8_t *read_exefs(const char *path, size_t *len, size_t max) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    void *file = savagewind_exefs_file_open(exefs, name);
    if (!file) return NULL;
    int64_t size = savagewind_exefs_file_size(file);
    if (size < 0 || (uint64_t)size > max || (uint64_t)size > SIZE_MAX) {
        savagewind_exefs_file_close(file);
        return NULL;
    }
    uint8_t *data = malloc(size ? (size_t)size : 1);
    if (!data || savagewind_exefs_file_pread(file, data, (size_t)size, 0) != size) {
        free(data);
        data = NULL;
    } else *len = (size_t)size;
    savagewind_exefs_file_close(file);
    return data;
}

typedef struct {
    const char *keys;
    const char *const *paths;
    size_t count;
    char *why;
    size_t why_size;
    void *exefs;
} PackageOpenTask;

static void *open_package_thread(void *arg) {
    PackageOpenTask *task = arg;
    uint64_t start = plat_now_ns();
    void *romfs = savagewind_package_open(task->keys, task->paths, task->count, 0, task->why, task->why_size);
    if (romfs) {
        fprintf(stderr, "[ps5] package RomFS opened: %llu bytes in %.3f s\n",
                (unsigned long long)savagewind_package_size(romfs), (plat_now_ns() - start) / 1e9);
        savagewind_package_close(romfs);
        task->exefs = savagewind_exefs_open(task->keys, task->paths, task->count, task->why, task->why_size);
    }
    return NULL;
}

static bool open_package_with_stack(PackageOpenTask *task) {
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc) { snprintf(task->why, task->why_size, "package thread attr init failed: %d", rc); return false; }
    rc = pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t thread;
    if (!rc) rc = pthread_create(&thread, &attr, open_package_thread, task);
    pthread_attr_destroy(&attr);
    if (rc) { snprintf(task->why, task->why_size, "package thread creation failed: %d", rc); return false; }
    fprintf(stderr, "[ps5] package open worker started (requested stack 8 MiB)\n");
    rc = pthread_join(thread, NULL);
    if (rc) { fprintf(stderr, "[ps5] package thread join failed: %d\n", rc); abort(); }
    return task->exefs != NULL;
}

bool ps5_exefs_init(const char *directory, char *why, size_t why_size) {
    DIR *d = opendir(directory);
    if (!d) { snprintf(why, why_size, "cannot open package directory (load ShadowMountPlus)"); return false; }
    char **paths = NULL;
    size_t count = 0;
    bool failed = false;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *ext = strrchr(e->d_name, '.');
        if (!ext || (strcasecmp(ext, ".nsp") && strcasecmp(ext, ".xci") &&
                     strcasecmp(ext, ".nsz") && strcasecmp(ext, ".xcz"))) continue;
        char **next = realloc(paths, (count + 1) * sizeof *paths);
        if (!next) { failed = true; break; }
        paths = next;
        size_t n = strlen(directory) + strlen(e->d_name) + 2;
        paths[count] = malloc(n);
        if (!paths[count]) { failed = true; break; }
        snprintf(paths[count++], n, "%s/%s", directory, e->d_name);
    }
    closedir(d);
    char keys[1200];
    int n = snprintf(keys, sizeof keys, "%s/prod.keys", directory);
    if (n < 0 || (size_t)n >= sizeof keys) failed = true;
    if (failed) snprintf(why, why_size, "unable to enumerate package directory");
    else {
        PackageOpenTask task = {keys, (const char *const *)paths, count, why, why_size, NULL};
        if (open_package_with_stack(&task)) exefs = task.exefs;
    }
    for (size_t i = 0; i < count; i++) free(paths[i]);
    free(paths);
    if (!exefs) return false;

    void *file = savagewind_exefs_file_open(exefs, "main");
    unsigned char header[0x60];
    bool valid = file && savagewind_exefs_file_pread(file, header, sizeof header, 0) == sizeof header &&
                 !memcmp(header, "NSO0", 4);
    savagewind_exefs_file_close(file);
    char id[65];
    if (valid) for (int i = 0; i < 32; i++) snprintf(id + i * 2, 3, "%02x", header[0x40 + i]);
    bool supported = false;
#define CHECK_VERSION(version, build_id, ok, files) \
    if (valid && ok && !strcmp(version, "1.9.0") && !strncmp(id, build_id, 16)) supported = true;
    GAME_VERSIONS(CHECK_VERSION)
#undef CHECK_VERSION
    const GameLibTables *tables = gamelib_tables();
    const char *linked_id = NULL;
    if (tables) for (uint32_t i = 0; i < *tables->nmodules; i++)
        if (!strcmp(tables->modules[i].name, "main")) linked_id = tables->modules[i].build_id;
    if (!supported || !linked_id || strcmp(id, linked_id)) {
        snprintf(why, why_size, "package main build ID does not match the linked 1.9.0 game code");
        savagewind_exefs_close(exefs);
        exefs = NULL;
        return false;
    }
    loader_set_file_reader(read_exefs);
    return true;
}
