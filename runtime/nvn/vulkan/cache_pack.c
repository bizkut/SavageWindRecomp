// Shader cache packs (cache_pack.h): what the shader precompilation (precompile.c) translates and compiles, in two
// files per cache folder instead of one file per shader - precompiled.pack (the entries' bytes one after another) and
// precompiled.idx (each entry's key, offset and size) in cache/xlate/<id>/ and cache/spirv/<id>/. An entry holds the
// bytes its single file would (shaders.c xlate_load, spirv.c cache_load read both the same way and check them the
// same way), so a damaged or missing pack only means misses. Shaders translated while playing still get files of
// their own.
//
// Why: making a file costs 11-15 ms on the PS5's /data whatever its size, through the storage broker one request at a
// time (ps5/FINDINGS.md 2026-10-27, "Root payload and storage broker": 64 KiB create/write/close 11.0 ms, durable
// replacement 15.5 ms), and the first start's precompilation made ~27,000 of them: its translation step took 370 s
// where the same work takes 47 s of CPU on 7 threads of the host. The packs are written in 8 MiB appends by a thread
// of their own (27 MiB/s through the broker: ~600 MB in ~25 s, while the workers translate).
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../paths.h"
#include "../../rt.h"
#include "cache_pack.h"
#include "shaders.h"
#ifdef __PROSPERO__
#include "../../storage_ps5.h"
#define mkdir(path, mode) rt_storage_mkdirs(path)
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

typedef struct { uint64_t key, off; uint32_t size, pad; } PackEntry;
typedef struct { char magic[4]; uint32_t version, count, pad; uint64_t bytes; } PackIndexHeader;
#define PACK_VERSION 1
#define PACK_BUFFER (8u << 20)  // bytes an append takes
#define PACK_QUEUE 4             // appends waiting at most (the adders wait beyond that)

typedef struct {
    bool loaded;
    PackEntry *tab;           // open addressing by key; size 0: free
    uint32_t cap, n;
    uint64_t bytes;           // the pack's bytes the index covers (written, readable)
    int fd;                   // the pack, for reads (-1: not open)
    // writing (cache_pack_begin .. cache_pack_end)
    uint64_t end;             // where the next entry added goes
    uint8_t *buf;
    size_t buf_n, buf_cap;
    PackEntry *pend;          // the entries in buf
    uint32_t npend, cap_pend;
    bool failed;              // an append failed: no more adds (the callers write files of their own)
} Pack;

typedef struct Job { int kind; uint8_t *data; size_t n; PackEntry *e; uint32_t ne; struct Job *next; } Job;

static Pack g_pack[PACK_KINDS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static Job *g_head, *g_tail;
static int g_queued;
static bool g_busy, g_stop, g_writing;
static pthread_t g_writer;

static void pack_path(int kind, const char *leaf, char *out, size_t n) {
    char name[96];
    if (kind == PACK_XLATE) snprintf(name, sizeof name, "xlate/%s%s%s", vk_cache_dir_id(maxwell_translator_id()), *leaf ? "/" : "", leaf);
    else snprintf(name, sizeof name, "spirv/%s%s%s", vk_cache_dir_id(vk_spirv_id()), *leaf ? "/" : "", leaf);
    vk_cache_path(out, n, name);
}

static void tab_put(Pack *p, PackEntry e) {  // (g_lock held)
    if ((p->n + 1) * 2 > p->cap) {
        uint32_t cap = p->cap ? p->cap * 2 : 1u << 14;
        PackEntry *t = calloc(cap, sizeof *t);
        if (!t) return;
        for (uint32_t i = 0; i < p->cap; i++) {
            if (!p->tab[i].size) continue;
            uint32_t j = (uint32_t)p->tab[i].key & (cap - 1);
            while (t[j].size) j = (j + 1) & (cap - 1);
            t[j] = p->tab[i];
        }
        free(p->tab);
        p->tab = t;
        p->cap = cap;
    }
    uint32_t j = (uint32_t)e.key & (p->cap - 1);
    for (; p->tab[j].size; j = (j + 1) & (p->cap - 1))
        if (p->tab[j].key == e.key) { p->tab[j] = e; return; }  // (the newer one)
    p->tab[j] = e;
    p->n++;
}

static const PackEntry *tab_find(const Pack *p, uint64_t key) {
    if (!p->cap) return NULL;
    for (uint32_t j = (uint32_t)key & (p->cap - 1); p->tab[j].size; j = (j + 1) & (p->cap - 1))
        if (p->tab[j].key == key) return &p->tab[j];
    return NULL;
}

static void tab_clear(Pack *p) {
    free(p->tab);
    p->tab = NULL;
    p->cap = p->n = 0;
    p->bytes = 0;
}

// The index, once (g_lock held): entries past the pack's end, or an index that doesn't match its pack, are dropped
static void pack_load(int kind) {
    Pack *p = &g_pack[kind];
    if (p->loaded) return;
    p->loaded = true;
    p->fd = -1;
    char path[512];
    pack_path(kind, "precompiled.idx", path, sizeof path);
    FILE *f = rt_data_fopen(path, "rb");
    if (!f) return;
    PackIndexHeader h;
    struct stat st;
    char pack[512];
    pack_path(kind, "precompiled.pack", pack, sizeof pack);
    bool ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "SWPK", 4) && h.version == PACK_VERSION &&
              !rt_data_stat(pack, &st) && (uint64_t)st.st_size >= h.bytes;
    PackEntry e;
    for (uint32_t i = 0; ok && i < h.count && fread(&e, sizeof e, 1, f) == 1; i++)
        if (e.size && e.off + e.size <= h.bytes) tab_put(p, e);
    fclose(f);
    if (ok) p->bytes = h.bytes;
    else tab_clear(p);
}

bool cache_pack_has(int kind, uint64_t key) {
    pthread_mutex_lock(&g_lock);
    pack_load(kind);
    bool has = tab_find(&g_pack[kind], key) != NULL;
    pthread_mutex_unlock(&g_lock);
    return has;
}

bool cache_pack_read(int kind, uint64_t key, uint8_t **data, size_t *size) {
    Pack *p = &g_pack[kind];
    pthread_mutex_lock(&g_lock);
    pack_load(kind);
    const PackEntry *e = tab_find(p, key);
    PackEntry got = e ? *e : (PackEntry){0};
    if (e && p->fd < 0) {
        char path[512], host[1024];
        pack_path(kind, "precompiled.pack", path, sizeof path);
        if (rt_data_path(path, host, sizeof host)) p->fd = open(host, O_RDONLY | O_BINARY);
    }
    int fd = p->fd;
    pthread_mutex_unlock(&g_lock);
    if (!got.size || fd < 0) return false;
    uint8_t *d = malloc(got.size);
    if (!d || pread(fd, d, got.size, (off_t)got.off) != (ssize_t)got.size) {
        free(d);
        return false;
    }
    *data = d;
    *size = got.size;
    return true;
}

// ---- writing
static bool append(int kind, const uint8_t *data, size_t n) {
    char path[512];
    pack_path(kind, "precompiled.pack", path, sizeof path);
#ifdef __PROSPERO__
    return rt_storage_append(path, data, n) == 0;
#else
    FILE *f = fopen(path, "ab");
    bool ok = f && fwrite(data, 1, n, f) == n;
    if (f && fclose(f)) ok = false;
    return ok;
#endif
}

static void *writer(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        while (!g_head && !g_stop) pthread_cond_wait(&g_cond, &g_lock);
        if (!g_head) break;
        Job *j = g_head;
        g_head = j->next;
        if (!g_head) g_tail = NULL;
        g_busy = true;
        Pack *p = &g_pack[j->kind];
        bool skip = p->failed || (j->ne && j->e[0].off != p->bytes);
        pthread_mutex_unlock(&g_lock);
        bool ok = !skip && append(j->kind, j->data, j->n);
        pthread_mutex_lock(&g_lock);
        if (ok) {
            for (uint32_t i = 0; i < j->ne; i++) tab_put(p, j->e[i]);
            p->bytes += j->n;
        } else if (!p->failed) {
            p->failed = true;
            LOG("[precompile] writing %s's pack failed: its entries go to files of their own from here\n",
                j->kind == PACK_XLATE ? "the translations" : "the SPIR-V");
        }
        free(j->data);
        free(j->e);
        free(j);
        g_queued--;
        g_busy = false;
        pthread_cond_broadcast(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void enqueue(int kind) {  // (g_lock held) what buf holds, to be appended
    Pack *p = &g_pack[kind];
    if (!p->buf_n) return;
    Job *j = malloc(sizeof *j);
    if (!j) { p->failed = true; return; }
    *j = (Job){kind, p->buf, p->buf_n, p->pend, p->npend, NULL};
    p->buf = NULL;
    p->buf_n = p->buf_cap = 0;
    p->pend = NULL;
    p->npend = p->cap_pend = 0;
    if (g_tail) g_tail->next = j;
    else g_head = j;
    g_tail = j;
    g_queued++;
    pthread_cond_broadcast(&g_cond);
}

void cache_pack_begin(void) {
    pthread_mutex_lock(&g_lock);
    for (int k = 0; k < PACK_KINDS; k++) {
        Pack *p = &g_pack[k];
        pack_load(k);
        char dir[512], pack[512], idx[512];
        pack_path(k, "", dir, sizeof dir);
        pack_path(k, "precompiled.pack", pack, sizeof pack);
        pack_path(k, "precompiled.idx", idx, sizeof idx);
        struct stat st;
        bool keep = p->bytes && !rt_data_stat(pack, &st) && (uint64_t)st.st_size == p->bytes;
        p->failed = false;
        if (!keep) {  // (an interrupted run's tail, or nothing yet: started over)
            tab_clear(p);
#ifdef __PROSPERO__
            rt_storage_unlink(idx);
            rt_storage_mkdirs(dir);
            p->failed = rt_storage_create(pack) != 0;
#else
            remove(idx);
            char parent[512];
            snprintf(parent, sizeof parent, "%s", dir);
            char *slash = strrchr(parent, '/');
            if (slash) *slash = 0, mkdir(parent, 0755);
            mkdir(dir, 0755);
            FILE *f = fopen(pack, "wb");
            p->failed = !f;
            if (f) fclose(f);
#endif
        }
        p->end = p->bytes;
        if (p->failed) LOG("[precompile] %s: can't be made, entries go to files of their own\n", pack);
    }
    g_stop = false;
    g_writing = pthread_create(&g_writer, NULL, writer, NULL) == 0;
    pthread_mutex_unlock(&g_lock);
}

static _Atomic uint32_t g_refused[PACK_KINDS];
static const char *g_refusal[PACK_KINDS];  // the first refusal's reason

static bool refuse(int kind, const char *why) {
    if (atomic_fetch_add(&g_refused[kind], 1) == 0) g_refusal[kind] = why;
    return false;
}

bool cache_pack_add(int kind, uint64_t key, const void *data, size_t n) {
    if (!__atomic_load_n(&g_writing, __ATOMIC_ACQUIRE)) return false;  // (not precompiling: a file of its own)
    if (kind < 0 || kind >= PACK_KINDS) return false;
    if (!n || n > UINT32_MAX) return refuse(kind, "size");
    Pack *p = &g_pack[kind];
    pthread_mutex_lock(&g_lock);
    bool ok = g_writing && !p->failed;
    if (!ok) refuse(kind, p->failed ? "the pack failed" : "writing ended");
    if (ok && p->buf_n + n > p->buf_cap) {
        size_t cap = p->buf_cap ? p->buf_cap : PACK_BUFFER;
        while (cap < p->buf_n + n) cap *= 2;
        uint8_t *b = realloc(p->buf, cap);
        if (b) p->buf = b, p->buf_cap = cap;
        else ok = refuse(kind, "no memory for the buffer");
    }
    if (ok && p->npend == p->cap_pend) {
        uint32_t cap = p->cap_pend ? p->cap_pend * 2 : 1024;
        PackEntry *e = realloc(p->pend, sizeof *e * cap);
        if (e) p->pend = e, p->cap_pend = cap;
        else ok = refuse(kind, "no memory for the entries");
    }
    if (ok) {
        memcpy(p->buf + p->buf_n, data, n);
        p->buf_n += n;
        p->pend[p->npend++] = (PackEntry){key, p->end, (uint32_t)n, 0};
        p->end += n;
        if (p->buf_n >= PACK_BUFFER) {
            enqueue(kind);
            while (g_queued >= PACK_QUEUE) pthread_cond_wait(&g_cond, &g_lock);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void cache_pack_flush(void) {
    pthread_mutex_lock(&g_lock);
    for (int k = 0; k < PACK_KINDS; k++) enqueue(k);
    while (g_writing && (g_head || g_busy)) pthread_cond_wait(&g_cond, &g_lock);
    pthread_mutex_unlock(&g_lock);
}

void cache_pack_end(void) {
    if (!__atomic_load_n(&g_writing, __ATOMIC_ACQUIRE)) return;
    cache_pack_flush();
    pthread_mutex_lock(&g_lock);
    g_stop = true;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_writer, NULL);
    pthread_mutex_lock(&g_lock);
    __atomic_store_n(&g_writing, false, __ATOMIC_RELEASE);
    for (int k = 0; k < PACK_KINDS; k++) {
        Pack *p = &g_pack[k];
        free(p->buf);
        free(p->pend);
        p->buf = NULL;
        p->pend = NULL;
        p->buf_n = p->buf_cap = 0;
        p->npend = p->cap_pend = 0;
        // the index of what was written (a failed append: up to it)
        size_t bytes = sizeof(PackIndexHeader) + (size_t)p->n * sizeof(PackEntry);
        uint8_t *idx = malloc(bytes);
        if (!idx) continue;
        PackIndexHeader h = {{'S', 'W', 'P', 'K'}, PACK_VERSION, 0, 0, p->bytes};
        PackEntry *e = (PackEntry *)(idx + sizeof h);
        for (uint32_t i = 0; i < p->cap; i++)
            if (p->tab[i].size) e[h.count++] = p->tab[i];
        memcpy(idx, &h, sizeof h);
        char path[512], pack[512];
        pack_path(k, "precompiled.idx", path, sizeof path);
        pack_path(k, "precompiled.pack", pack, sizeof pack);
#ifdef __PROSPERO__
        bool ok = rt_storage_fsync(pack) == 0 && rt_storage_replace(path, idx, bytes) == 0;
#else
        char tmp[540];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        bool ok = f && fwrite(idx, 1, bytes, f) == bytes;
        if (f && fclose(f)) ok = false;
        if (!ok || !vk_replace_file(tmp, path)) remove(tmp), ok = false;
#endif
        free(idx);
        uint32_t refused = atomic_load(&g_refused[k]);
        LOG("[precompile] %s: %u entries, %.0f MB%s; %u stores refused%s%s\n", pack, h.count, p->bytes / 1048576.0,
            ok ? "" : " (its index NOT saved)", refused, refused ? ", the first: " : "", refused ? g_refusal[k] : "");
    }
    pthread_mutex_unlock(&g_lock);
}
