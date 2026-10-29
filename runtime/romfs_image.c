// The RomFS image served from the romfs/ folder (romfs_image.h). The game reads from several threads, so a read only
// holds the lock to find its files' descriptors: the pread runs outside it, and a descriptor in use is never closed.
#include "romfs_image.h"

#include "platform.h"  // (Windows: pread)
#include "romfs_build.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifdef ROMFS_PACKAGE
extern void *savagewind_package_open(const char *, const char *const *, size_t, unsigned, char *, size_t);
extern uint64_t savagewind_package_size(void *);
extern ssize_t savagewind_package_pread(void *, void *, size_t, uint64_t);
#ifdef __PROSPERO__
extern void savagewind_package_set_coalesce(int);
extern void savagewind_package_set_broker(int);
#include "settings.h"
#endif
extern void savagewind_package_close(void *);
#endif

// Files kept open (macOS allows 256 open files by default, and the overlay keeps its own open: runtime/services/fs.c)
#define OPEN_FILES 32

typedef struct {
    int file, fd, busy;  // busy: reads using fd right now
    uint64_t used;       // for closing the least recently used
} OpenFile;

struct RomfsImage {
    RomfsLayout layout;
    int bin_fd;  // an older version's romfs.bin; -1: served from the folder
    uint64_t size;
    pthread_mutex_t lock;
    OpenFile open[OPEN_FILES];
    uint64_t tick;
#ifdef ROMFS_PACKAGE
    void *package;
#endif
};

RomfsImage *romfs_image_open(const char *dir, char *why, size_t why_size) {
    RomfsImage *img = calloc(1, sizeof *img);
    img->bin_fd = -1;
    pthread_mutex_init(&img->lock, NULL);
    for (int k = 0; k < OPEN_FILES; k++) img->open[k].fd = -1;
    struct stat sb;
    char layout_why[256] = "";
    if (stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode) && romfs_layout(dir, &img->layout, layout_why, sizeof layout_why)) {
        img->size = img->layout.size;
        return img;
    }
    char bin[1100];
    snprintf(bin, sizeof bin, "%s.bin", dir);
    img->bin_fd = open(bin, O_RDONLY | O_BINARY);
    if (img->bin_fd >= 0 && fstat(img->bin_fd, &sb) == 0) {
        img->size = (uint64_t)sb.st_size;
        return img;
    }
    if (why) snprintf(why, why_size, "%s", layout_why[0] ? layout_why : "no game data folder");
    romfs_image_close(img);
    return NULL;
}

#ifdef ROMFS_PACKAGE
RomfsImage *romfs_image_open_package(const char *keys, const char *const *packages, size_t count,
                                     unsigned addon, char *why, size_t why_size) {
#ifdef __PROSPERO__
    savagewind_package_set_coalesce(g_settings.package_read_coalesce);
    savagewind_package_set_broker(g_settings.package_read_broker);
#endif
    void *package = savagewind_package_open(keys, packages, count, addon, why, why_size);
    if (!package) return NULL;
    return romfs_image_adopt_package(package, why, why_size);
}

RomfsImage *romfs_image_adopt_package(void *package, char *why, size_t why_size) {
    if (!package) return NULL;
    uint64_t size = savagewind_package_size(package);
    if (size == UINT64_MAX) {
        if (why && why_size) snprintf(why, why_size, "cannot query package RomFS size");
        savagewind_package_close(package);
        return NULL;
    }
    RomfsImage *img = calloc(1, sizeof *img);
    if (!img) {
        savagewind_package_close(package);
        return NULL;
    }
    img->bin_fd = -1;
    img->package = package;
    img->size = size;
    pthread_mutex_init(&img->lock, NULL);
    for (int k = 0; k < OPEN_FILES; k++) img->open[k].fd = -1;
    return img;
}
#endif

uint64_t romfs_image_size(const RomfsImage *img) { return img->size; }

// The descriptor of file i, marked busy in its slot (*slot; -1: every slot busy, the caller closes it). lock held.
static int file_fd(RomfsImage *img, int i, int *slot) {
    int pick = -1;
    for (int k = 0; k < OPEN_FILES; k++) {
        OpenFile *o = &img->open[k];
        if (o->fd >= 0 && o->file == i) {
            o->busy++, o->used = ++img->tick, *slot = k;
            return o->fd;
        }
        if (!o->busy && (pick < 0 || o->used < img->open[pick].used)) pick = k;
    }
    int fd = open(img->layout.files[i].path, O_RDONLY | O_BINARY);
    *slot = fd >= 0 ? pick : -1;
    if (*slot < 0) return fd;
    if (img->open[pick].fd >= 0) close(img->open[pick].fd);
    img->open[pick] = (OpenFile){i, fd, 1, ++img->tick};
    return fd;
}

static bool read_file(RomfsImage *img, int i, uint8_t *dst, uint64_t n, uint64_t off) {
    int slot;
    pthread_mutex_lock(&img->lock);
    int fd = file_fd(img, i, &slot);
    pthread_mutex_unlock(&img->lock);
    if (fd < 0) return false;
    bool ok = pread(fd, dst, n, (off_t)off) == (ssize_t)n;
    pthread_mutex_lock(&img->lock);
    if (slot >= 0) img->open[slot].busy--;
    else close(fd);
    pthread_mutex_unlock(&img->lock);
    return ok;
}

ssize_t romfs_image_pread(RomfsImage *img, void *dst_, uint64_t n, uint64_t off) {
#ifdef ROMFS_PACKAGE
    if (img->package) {
        uint64_t available = off < img->size ? img->size - off : 0;
        size_t take = n < available ? (size_t)n : (size_t)available;
        if (take && savagewind_package_pread(img->package, dst_, take, off) != (ssize_t)take) return -1;
        memset((uint8_t *)dst_ + take, 0, n - take);
        return (ssize_t)n;
    }
#endif
    if (img->bin_fd >= 0) return pread(img->bin_fd, dst_, n, (off_t)off);
    const RomfsLayout *l = &img->layout;
    uint8_t *dst = dst_;
    memset(dst, 0, n);
    uint64_t end = off + n;
    if (off < ROMFS_DATA_START) memcpy(dst, l->header + off, (end < ROMFS_DATA_START ? end : ROMFS_DATA_START) - off);
    uint64_t ta = l->tables_off, tb = ta + l->tables_size;
    if (off < tb && end > ta) {
        uint64_t a = off > ta ? off : ta, b = end < tb ? end : tb;
        memcpy(dst + (a - off), l->tables + (a - ta), b - a);
    }
    // the files overlapping [off, end): the first by binary search, then in order
    int lo = 0, hi = l->nfiles - 1, first = l->nfiles;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (l->files[mid].start + l->files[mid].size > off) first = mid, hi = mid - 1;
        else lo = mid + 1;
    }
    for (int i = first; i < l->nfiles && l->files[i].start < end; i++) {
        const RomfsLayoutFile *f = &l->files[i];
        uint64_t a = off > f->start ? off : f->start, b = end < f->start + f->size ? end : f->start + f->size;
        if (a >= b) continue;
        if (!read_file(img, i, dst + (a - off), b - a, a - f->start)) {
            fprintf(stderr, "[romfs] can't read %s\n", f->path);
            return -1;
        }
    }
    return (ssize_t)n;
}

void romfs_image_close(RomfsImage *img) {
    if (!img) return;
#ifdef ROMFS_PACKAGE
    if (img->package) savagewind_package_close(img->package);
#endif
    for (int k = 0; k < OPEN_FILES; k++)
        if (img->open[k].fd >= 0) close(img->open[k].fd);
    if (img->bin_fd >= 0) close(img->bin_fd);
    romfs_layout_free(&img->layout);
    pthread_mutex_destroy(&img->lock);
    free(img);
}
