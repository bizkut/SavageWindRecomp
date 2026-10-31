// Read the linked game's ExeFS directly from the user's packages.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gamelib.h"
#include "../game_versions.h"
#include "../rt.h"
#include "../romfs_image.h"
#include "../settings.h"
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
int ps5_exefs_home_art_identity(unsigned char out[128]) {
    return savagewind_exefs_home_art_identity(exefs, out);
}
static uint64_t exefs_bytes, exefs_read_ns, exefs_reads;
#ifdef GAMECODE_CONSOLE
// ExeFS streams can share seekable package state; shader and generator reads must not overlap.
static pthread_mutex_t exefs_io_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
static struct {
    pthread_t thread;
    char keys[1200], why[512];
    char **paths;
    size_t count;
    int addons;
    void *package;
    bool started, joined, success;
} startup_romfs;

extern void savagewind_package_set_coalesce(int);
extern void savagewind_package_set_broker(int);
extern int savagewind_package_open_addons(void *);

static void *open_startup_romfs(void *unused) {
    (void)unused;
    plat_thread_heap_own();
    uint64_t start = plat_now_ns();
    startup_romfs.package = savagewind_package_open(startup_romfs.keys, (const char *const *)startup_romfs.paths,
                                                     startup_romfs.count, 0, startup_romfs.why, sizeof startup_romfs.why);
    ps5_startup_mark("startup RomFS open finished");
    if (startup_romfs.package)
        fprintf(stderr, "[ps5] package RomFS opened: %llu bytes in %.3f s\n",
                (unsigned long long)savagewind_package_size(startup_romfs.package), (plat_now_ns() - start) / 1e9);
    return NULL;
}
bool ps5_startup_romfs_enabled(void) { return startup_romfs.started; }


bool ps5_startup_romfs_wait(char *why, size_t why_size) {
    if (!startup_romfs.started) return true;
    if (!startup_romfs.joined) {
        int rc = pthread_join(startup_romfs.thread, NULL);
        if (rc) { fprintf(stderr, "[ps5] package thread join failed: %d\n", rc); abort(); }
        startup_romfs.joined = true;
        startup_romfs.success = startup_romfs.package != NULL;
        startup_romfs.addons = savagewind_package_open_addons(startup_romfs.package);
        for (size_t i = 0; i < startup_romfs.count; i++) free(startup_romfs.paths[i]);
        free(startup_romfs.paths);
        startup_romfs.paths = NULL;
    }
    if (!startup_romfs.success) {
        snprintf(why, why_size, "%s", startup_romfs.why);
        return false;
    }
    return true;
}
int ps5_startup_romfs_addons(void) {
    char why[512];
    return startup_romfs.started && ps5_startup_romfs_wait(why, sizeof why) ? startup_romfs.addons : -1;
}


RomfsImage *ps5_startup_romfs_take(char *why, size_t why_size) {
    if (!ps5_startup_romfs_wait(why, why_size) || !startup_romfs.package) return NULL;
    savagewind_package_set_coalesce(g_settings.package_read_coalesce);
    savagewind_package_set_broker(g_settings.package_read_broker);
    void *package = startup_romfs.package;
    startup_romfs.package = NULL;
    return romfs_image_adopt_package(package, why, why_size);
}

void ps5_exefs_read_summary(void) {
    fprintf(stderr, "[ps5] ExeFS file reads: %llu files, %llu bytes in %.3f s\n",
            (unsigned long long)exefs_reads, (unsigned long long)exefs_bytes, exefs_read_ns / 1e9);
}


static uint8_t *read_exefs(const char *path, size_t *len, size_t max) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
#ifdef GAMECODE_CONSOLE
    pthread_mutex_lock(&exefs_io_lock);
#endif
    void *file = savagewind_exefs_file_open(exefs, name);
    if (!file) {
#ifdef GAMECODE_CONSOLE
        pthread_mutex_unlock(&exefs_io_lock);
#endif
        return NULL;
    }
    int64_t size = savagewind_exefs_file_size(file);
    if (size < 0 || (uint64_t)size > max || (uint64_t)size > SIZE_MAX) {
        savagewind_exefs_file_close(file);
#ifdef GAMECODE_CONSOLE
        pthread_mutex_unlock(&exefs_io_lock);
#endif
        return NULL;
    }
    uint64_t start = plat_now_ns();
    uint8_t *data = malloc(size ? (size_t)size : 1);
    if (!data || savagewind_exefs_file_pread(file, data, (size_t)size, 0) != size) {
        free(data);
        data = NULL;
    } else *len = (size_t)size;
    savagewind_exefs_file_close(file);
    exefs_reads++;
    exefs_bytes += (uint64_t)size;
    exefs_read_ns += plat_now_ns() - start;
#ifdef GAMECODE_CONSOLE
    pthread_mutex_unlock(&exefs_io_lock);
#endif
    return data;
}
#ifdef GAMECODE_CONSOLE
uint8_t *ps5_exefs_read_module(const char *name, size_t *size) {
    return read_exefs(name, size, 256u * 1024u * 1024u);
}

bool ps5_exefs_validate_gamecode(char *why, size_t why_size) {
    static const char *const names[] = {"main", "subsdk0", "sdk"};
    const GameLibTables *tables = gamelib_tables();
    if (!tables || *tables->nmodules != sizeof names / sizeof names[0]) {
        snprintf(why, why_size, "game code module layout is invalid");
        return false;
    }
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const GameLibModule *module = &tables->modules[i];
        void *file = savagewind_exefs_file_open(exefs, names[i]);
        unsigned char header[0x60];
        bool valid = file && savagewind_exefs_file_pread(file, header, sizeof header, 0) == sizeof header &&
                     !memcmp(header, "NSO0", 4);
        savagewind_exefs_file_close(file);
        char id[65];
        if (valid) for (size_t j = 0; j < 32; j++) snprintf(id + j * 2, 3, "%02x", header[0x40 + j]);
        if (!valid || !module->name || strcmp(module->name, names[i]) || !module->build_id ||
            strcmp(module->build_id, id)) {
            snprintf(why, why_size, "package %s build ID does not match game code", names[i]);
            return false;
        }
    }
    return true;
}
#endif

typedef struct {
    const char *keys;
    const char *const *paths;
    size_t count;
    char *why;
    size_t why_size;
    void *exefs;
    bool fast;
} PackageOpenTask;

static void *open_package_thread(void *arg) {
    PackageOpenTask *task = arg;
    if (!task->fast) {
        uint64_t start = plat_now_ns();
        void *romfs = savagewind_package_open(task->keys, task->paths, task->count, 0, task->why, task->why_size);
        ps5_startup_mark("startup RomFS open finished");
        if (!romfs) return NULL;
        fprintf(stderr, "[ps5] package RomFS opened: %llu bytes in %.3f s\n",
                (unsigned long long)savagewind_package_size(romfs), (plat_now_ns() - start) / 1e9);
        savagewind_package_close(romfs);
    }
    task->exefs = savagewind_exefs_open(task->keys, task->paths, task->count, task->why, task->why_size);
    ps5_startup_mark("ExeFS open finished");
    if (task->fast && task->exefs) {
        pthread_attr_t attr;
        int rc = pthread_attr_init(&attr);
        if (!rc) {
            rc = pthread_attr_setstacksize(&attr, 8u << 20);
            if (!rc) rc = pthread_create(&startup_romfs.thread, &attr, open_startup_romfs, NULL);
            pthread_attr_destroy(&attr);
        }
        if (rc) {
            snprintf(task->why, task->why_size, "package thread creation failed: %d", rc);
            savagewind_exefs_close(task->exefs);
            task->exefs = NULL;
        } else startup_romfs.started = true;
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
    savagewind_package_set_startup_mark(ps5_startup_mark);
    savagewind_package_set_fast_exefs(settings_fast_exefs_enabled());
    DIR *d = opendir(directory);
    if (!d) {
        snprintf(why, why_size, "The folder %s doesn't exist. Create it and put your game files in it.", directory);
        return false;
    }
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
    if (!failed && !count) {
        // The usual mistake: the packages copied one level up (the data root) instead of into the import folder
        char misplaced[256] = "";
        DIR *up = opendir("/data/savagewind");
        while (up && (e = readdir(up)) && !misplaced[0]) {
            const char *ext = strrchr(e->d_name, '.');
            if (ext && (!strcasecmp(ext, ".nsp") || !strcasecmp(ext, ".xci")))
                snprintf(misplaced, sizeof misplaced, "%s", e->d_name);
        }
        if (up) closedir(up);
        // (package names are long: the TV line names the folders, the log names the file)
        if (misplaced[0]) {
            fprintf(stderr, "[ps5] package outside the import folder: /data/savagewind/%s\n", misplaced);
            snprintf(why, why_size, "Game files are in /data/savagewind/: move them into %s/.", directory);
        } else {
            snprintf(why, why_size, "No NSP or XCI files in %s/.", directory);
        }
        free(paths);
        return false;
    }
    char keys[1200];
    int n = snprintf(keys, sizeof keys, "%s/prod.keys", directory);
    if (n < 0 || (size_t)n >= sizeof keys) failed = true;
    if (failed) snprintf(why, why_size, "unable to enumerate package directory");
    else {
        bool fast = settings_fast_start_enabled();
        if (fast) {
            snprintf(startup_romfs.keys, sizeof startup_romfs.keys, "%s", keys);
            startup_romfs.paths = paths;
            startup_romfs.count = count;
        }
        PackageOpenTask task = {keys, (const char *const *)paths, count, why, why_size, NULL, fast};
        if (open_package_with_stack(&task)) exefs = task.exefs;
    }
    if (!startup_romfs.started) {
        for (size_t i = 0; i < count; i++) free(paths[i]);
        free(paths);
    }
    if (!exefs) return false;

    void *file = savagewind_exefs_file_open(exefs, "main");
    unsigned char header[0x60];
    bool valid = file && savagewind_exefs_file_pread(file, header, sizeof header, 0) == sizeof header &&
                 !memcmp(header, "NSO0", 4);
    savagewind_exefs_file_close(file);
    ps5_startup_mark("ExeFS build ID read finished");
    char id[65];
    if (valid) for (int i = 0; i < 32; i++) snprintf(id + i * 2, 3, "%02x", header[0x40 + i]);
    bool supported = false;
#define CHECK_VERSION(version, build_id, ok, files) \
    if (valid && ok && !strcmp(version, "1.9.0") && !strncmp(id, build_id, 16)) supported = true;
    GAME_VERSIONS(CHECK_VERSION)
#undef CHECK_VERSION
#ifndef GAMECODE_CONSOLE
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
#else
    if (!supported) {
        snprintf(why, why_size, "package main is not the supported 1.9.0 game");
        savagewind_exefs_close(exefs);
        exefs = NULL;
        return false;
    }
#endif
    loader_set_file_reader(read_exefs);
    return true;
}
