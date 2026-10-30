// Build or load the player's game code without embedding it in the title.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gamecode_cache.h"
#include "../gamecode_compile.h"
#include "../gamecode_exports.h"
#include "../gamelib.h"
#include "../platform.h"
#include "../storage_ps5.h"
#include <mbedtls/sha256.h>
#include <ps5platform/exec.h>
#include <ps5platform/heap.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CACHE_DIR "cache/gamecode"
#define CACHE_IMAGE CACHE_DIR "/image.bin"
#define CACHE_MANIFEST CACHE_DIR "/manifest"
#define CACHE_ROOT "/data/savagewind/"
#define INCLUDE_ROOT "/app0/gamecode-include"
#define MAX_CACHE_BYTES ((size_t)2 * 1024 * 1024 * 1024)

extern uint8_t *ps5_exefs_read_module(const char *, size_t *);
extern void ps5_gamelib_set_image(const GameLinkImage *);

static struct ps5_exec_region region;
static GameLinkImage loaded_image;

static int decode_sha(const char *text, unsigned char out[32]) {
    if (!text || strlen(text) != 64) return 0;
    for (size_t i = 0; i < 32; i++) {
        unsigned a, b;
        char x = text[i * 2], y = text[i * 2 + 1];
        a = x >= '0' && x <= '9' ? x - '0' : x >= 'a' && x <= 'f' ? x - 'a' + 10 : 16;
        b = y >= '0' && y <= '9' ? y - '0' : y >= 'a' && y <= 'f' ? y - 'a' + 10 : 16;
        if (a > 15 || b > 15) return 0;
        out[i] = (unsigned char)((a << 4) | b);
    }
    return 1;
}

static int hash_bytes(const void *data, size_t size, unsigned char out[32]) {
    return mbedtls_sha256_ret(data, size, out, 0) == 0;
}

static void *read_file(const char *path, size_t max, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 0 || (uint64_t)st.st_size > max) {
        close(fd);
        return NULL;
    }
    size_t n = (size_t)st.st_size, done = 0;
    void *data = malloc(n ? n : 1);
    if (!data) { close(fd); return NULL; }
    while (done < n) {
        ssize_t got = read(fd, (char *)data + done, n - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { free(data); close(fd); return NULL; }
        done += (size_t)got;
    }
    close(fd);
    *size = n;
    return data;
}

static int allocate_image(void *context, size_t size, size_t alignment, void **rw, uintptr_t *rx) {
    (void)context;
    if (alignment > 65536 || region.base) return 0;
    struct ps5_exec_request request = {size, 0, (uintptr_t)&allocate_image, PS5_EXEC_NEAR | PS5_EXEC_DUAL_VIEW};
    int rc = ps5_exec_alloc(&request, &region);
    if (rc) {
        fprintf(stderr, "[gamecode] executable allocation failed: %#x\n", (unsigned)rc);
        return 0;
    }
    *rw = region.write_view;
    *rx = (uintptr_t)region.base;
    return 1;
}

static void release_image(void *context, void *rw, size_t size) {
    (void)context;
    (void)rw;
    (void)size;
    ps5_exec_free(&region);
}

struct object_header {
    unsigned char magic[8];
    uint64_t size;
    unsigned char sha256[32];
};
static const unsigned char object_magic[8] = {'S','W','O','B','J','0','0','1'};
struct object_stats { size_t hits, misses; };

static void object_path(const unsigned char key[32], char *path, size_t size) {
    static const char hex[] = "0123456789abcdef";
    char name[65];
    for (size_t i = 0; i < 32; i++) {
        name[i * 2] = hex[key[i] >> 4];
        name[i * 2 + 1] = hex[key[i] & 15];
    }
    name[64] = 0;
    snprintf(path, size, CACHE_DIR "/obj/%s.o", name);
}

static int object_lookup(void *context, const uint8_t key[32], const void **data, size_t *size) {
    struct object_stats *stats = context;
    char relative[128], path[256], checksum_path[272];
    object_path(key, relative, sizeof relative);
    snprintf(path, sizeof path, CACHE_ROOT "%s", relative);
    snprintf(checksum_path, sizeof checksum_path, "%s.sha256", path);
    size_t n, checksum_size;
    unsigned char *buffer = read_file(path, 256u * 1024u * 1024u, &n);
    unsigned char *checksum = read_file(checksum_path, sizeof(struct object_header), &checksum_size);
    struct object_header header;
    unsigned char digest[32];
    if (!buffer || !checksum || checksum_size != sizeof header) {
        free(checksum);
        goto miss;
    }
    memcpy(&header, checksum, sizeof header);
    free(checksum);
    if (memcmp(header.magic, object_magic, sizeof object_magic) || header.size != n ||
        !hash_bytes(buffer, n, digest) || memcmp(digest, header.sha256, 32)) goto miss;
    *data = buffer;
    *size = n;
    stats->hits++;
    return 1;
miss:
    free(buffer);
    stats->misses++;
    return 0;
}

static void object_release(void *context, const void *data, size_t size) {
    (void)context;
    (void)size;
    free((void *)data);
}

static int object_store(void *context, const uint8_t key[32], const void *data, size_t size) {
    (void)context;
    struct object_header header = {{0}, size, {0}};
    memcpy(header.magic, object_magic, sizeof object_magic);
    if (!hash_bytes(data, size, header.sha256)) return -1;
    char path[128], checksum_path[144];
    object_path(key, path, sizeof path);
    snprintf(checksum_path, sizeof checksum_path, "%s.sha256", path);
    if (rt_storage_replace(path, data, size) < 0 ||
        rt_storage_replace(checksum_path, &header, sizeof header) < 0) return -1;
    return 0;
}

struct memory_writer { unsigned char *data; size_t size, capacity; };
static int cache_write(void *context, uint64_t offset, const void *data, size_t size) {
    struct memory_writer *writer = context;
    if (offset > MAX_CACHE_BYTES || size > MAX_CACHE_BYTES - (size_t)offset) return 0;
    size_t end = (size_t)offset + size;
    if (end > writer->capacity) {
        size_t capacity = writer->capacity ? writer->capacity : 65536;
        while (capacity < end) capacity = capacity > MAX_CACHE_BYTES / 2 ? MAX_CACHE_BYTES : capacity * 2;
        void *next = realloc(writer->data, capacity);
        if (!next) return 0;
        writer->data = next;
        writer->capacity = capacity;
    }
    if (end > writer->size) {
        memset(writer->data + writer->size, 0, end - writer->size);
        writer->size = end;
    }
    memcpy(writer->data + (size_t)offset, data, size);
    return 1;
}

struct memory_reader { const unsigned char *data; size_t size; };
static int cache_read(void *context, uint64_t offset, void *data, size_t size) {
    struct memory_reader *reader = context;
    if (offset > reader->size || size > reader->size - (size_t)offset) return 0;
    memcpy(data, reader->data + (size_t)offset, size);
    return 1;
}

struct cache_manifest { unsigned char magic[8]; uint64_t size; unsigned char sha256[32]; };
static const unsigned char manifest_magic[8] = {'S','W','G','C','0','0','0','1'};
static void log_heap(const char *stage) {
    struct ps5_heap_stats heap;
    ps5_heap_stats(&heap);
    fprintf(stderr, "[gamecode] heap %s mapped=%llu peak=%llu segments=%u\n", stage,
            (unsigned long long)heap.mapped_bytes, (unsigned long long)heap.peak_bytes, heap.segments);
}


static int load_cache(const GameCodeIdentity *identity, const GameLinkAllocator *allocator,
                      GameLinkImage *image, char *why, size_t why_size) {
    size_t manifest_size, image_size;
    unsigned char *manifest_data = read_file(CACHE_ROOT CACHE_MANIFEST, sizeof(struct cache_manifest), &manifest_size);
    if (!manifest_data) return 0;
    struct cache_manifest manifest;
    if (manifest_size != sizeof manifest) { free(manifest_data); return 0; }
    memcpy(&manifest, manifest_data, sizeof manifest);
    free(manifest_data);
    if (memcmp(manifest.magic, manifest_magic, sizeof manifest_magic) || manifest.size > MAX_CACHE_BYTES) return 0;
    uint64_t start = plat_now_ns();
    unsigned char *bytes = read_file(CACHE_ROOT CACHE_IMAGE, MAX_CACHE_BYTES, &image_size);
    uint64_t read_end = plat_now_ns();
    unsigned char digest[32];
    if (!bytes) return 0;
    int valid = image_size == manifest.size && hash_bytes(bytes, image_size, digest) &&
                !memcmp(digest, manifest.sha256, 32);
    uint64_t hash_end = plat_now_ns();
    if (valid) {
        struct memory_reader input = {bytes, image_size};
        GameCodeCacheReader reader = {&input, image_size, cache_read};
        valid = gamecode_cache_load(identity, &reader, allocator, game_exports, game_export_count, image, why,
                                    why_size);
    }
    uint64_t load_end = plat_now_ns();
    fprintf(stderr, "[gamecode] cache load detail bytes=%zu read=%.3f s hash=%.3f s validate_relocate=%.3f s\n",
            image_size, (read_end - start) / 1e9, (hash_end - read_end) / 1e9, (load_end - hash_end) / 1e9);
    log_heap("cache loaded");
    free(bytes);
    return valid;
}

static int save_cache(const GameLinkImage *image, const GameCodeIdentity *identity, char *why, size_t why_size) {
    struct memory_writer output = {0};
    GameCodeCacheWriter writer = {&output, cache_write};
    if (!gamecode_cache_save(image, identity, &writer, why, why_size)) { free(output.data); return 0; }
    struct cache_manifest manifest = {{0}, output.size, {0}};
    memcpy(manifest.magic, manifest_magic, sizeof manifest_magic);
    if (!hash_bytes(output.data, output.size, manifest.sha256) ||
        rt_storage_replace(CACHE_IMAGE, output.data, output.size) < 0 ||
        rt_storage_replace(CACHE_MANIFEST, &manifest, sizeof manifest) < 0) {
        snprintf(why, why_size, "cannot atomically write game code cache through storage broker");
        free(output.data);
        return 0;
    }
    free(output.data);
    return 1;
}

static size_t image_remaining(const GameLinkImage *image, const void *pointer) {
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t bases[] = {(uintptr_t)image->rw, image->rx};
    for (size_t i = 0; i < 2; i++)
        if (address >= bases[i] && address - bases[i] < image->size)
            return image->size - (size_t)(address - bases[i]);
    return 0;
}

static int image_range(const GameLinkImage *image, const void *pointer, size_t size, size_t alignment) {
    return pointer && !((uintptr_t)pointer & (alignment - 1)) && image_remaining(image, pointer) >= size;
}

static int image_string(const GameLinkImage *image, const char *text) {
    size_t remaining = image_remaining(image, text);
    return remaining && memchr(text, 0, remaining) != NULL;
}

static int validate_layout(const GameLinkImage *image, const GameCodeIdentity *identity) {
    static const char *const names[] = {"main", "subsdk0", "sdk"};
    static const char hex[] = "0123456789abcdef";
    const uint32_t *module_count = image->game_module_count;
    const uint32_t *site_count = image->game_site_count;
    const GameLibModule *modules = image->game_modules;
    const GameLibSite *sites = image->game_sites;
    if (!image_range(image, module_count, sizeof *module_count, _Alignof(uint32_t)) ||
        !image_range(image, site_count, sizeof *site_count, _Alignof(uint32_t)) ||
        !image_string(image, image->game_version) || strcmp(image->game_version, "1.9.0") ||
        *module_count != 3 || !image_range(image, modules, 3 * sizeof *modules, _Alignof(GameLibModule)) ||
        *site_count > image->size / sizeof *sites ||
        !image_range(image, sites, (size_t)*site_count * sizeof *sites, _Alignof(GameLibSite)))
        return 0;
    for (size_t i = 0; i < 3; i++) {
        const GameLibModule *module = &modules[i];
        if (!image_string(image, module->name) || strcmp(module->name, names[i]) ||
            !image_string(image, module->build_id) || strlen(module->build_id) != 64 ||
            !image_range(image, module->count, sizeof *module->count, _Alignof(uint64_t)))
            return 0;
        for (size_t j = 0; j < 32; j++) {
            unsigned byte = identity->module_build_ids[i][j];
            if (module->build_id[j * 2] != hex[byte >> 4] || module->build_id[j * 2 + 1] != hex[byte & 15])
                return 0;
        }
        uint64_t count = *module->count;
        if (count > image->size / sizeof(*module->addrs) || count > image->size / sizeof(*module->fns) ||
            !image_range(image, module->addrs, (size_t)count * sizeof(*module->addrs), _Alignof(uint64_t)) ||
            !image_range(image, module->fns, (size_t)count * sizeof(*module->fns), _Alignof(GuestFn)))
            return 0;
        for (size_t j = 0; j < count; j++) {
            uintptr_t function = (uintptr_t)module->fns[j];
            if (function < image->rx || function - image->rx >= image->text_size) return 0;
        }
    }
    for (size_t i = 0; i < *site_count; i++)
        if (!image_string(image, sites[i].name)) return 0;
    return 1;
}

struct progress_state {
    GameCodeProgress callback;
    void *context;
    uint64_t generation_start;
};

static void report_progress(void *context, const char *stage, size_t done, size_t total) {
    struct progress_state *state = context;
    if (!strcmp(stage, "generate")) {
        if (!state->generation_start) state->generation_start = plat_now_ns();
        if (total && done == total)
            fprintf(stderr, "[gamecode] generating %.3f s\n", (plat_now_ns() - state->generation_start) / 1e9);
    }
    if ((!strcmp(stage, "compile") || !strcmp(stage, "cache")) && done && done % 100 == 0)
        fprintf(stderr, "[gamecode] compiling %zu of %zu%s\n", done, total ? total : 996,
                !strcmp(stage, "cache") ? " (cache hit)" : "");
    if (state->callback) state->callback(state->context, stage, done, total);
}

bool ps5_gamecode_prepare(GameCodeProgress progress, void *context, char *why, size_t why_size) {
    static const char *const names[] = {"main", "subsdk0", "sdk"};
    uint64_t start = plat_now_ns(), stage = start;
    GameCodeIdentity identity = {0};
    GameCodeCompileInputs inputs = {0};
    GameCodeObjects objects = {0};
    struct object_stats stats = {0};
    GameCodeObjectCache cache = {&stats, object_lookup, object_release, object_store};
    struct progress_state progress_state = {progress, context, 0};
    GameLinkAllocator allocator = {NULL, allocate_image, release_image};
    int ok = 0;
    if (why && why_size) why[0] = 0;
    if (rt_storage_mkdirs(CACHE_DIR "/obj") < 0) {
        snprintf(why, why_size, "cannot create game code cache through storage broker");
        goto done;
    }
    for (size_t i = 0; i < 3; i++) {
        size_t size = 0;
        unsigned char *data = ps5_exefs_read_module(names[i], &size);
        inputs.modules[i].data = data;
        inputs.modules[i].size = size;
        if (!data || size < 0x60 || memcmp(data, "NSO0", 4) || !hash_bytes(data, size, identity.module_sha256[i])) {
            snprintf(why, why_size, "cannot read valid package ExeFS module %s", names[i]);
            goto done;
        }
        memcpy(identity.module_build_ids[i], data + 0x40, 32);
    }
    if (!decode_sha(GAMECODE_GENERATOR_SHA256, identity.generator_version) ||
        !decode_sha(GAMECODE_COMPILE_SHA256, identity.compile_flags_sha256) ||
        !decode_sha(GAMECODE_LLVM_SHA256, identity.llvm_build_sha256) ||
        !decode_sha(game_export_abi_digest, identity.export_abi_sha256)) {
        snprintf(why, why_size, "invalid game code build identity");
        goto done;
    }
    snprintf(identity.llvm_version, sizeof identity.llvm_version, "18.1.8");
    identity.linker_version = GAMECODE_LINKER_VERSION;
    identity.cache_format_version = GAMECODE_CACHE_FORMAT_VERSION;
    memcpy(inputs.llvm_build_identity, identity.llvm_build_sha256, sizeof inputs.llvm_build_identity);
    fprintf(stderr, "[gamecode] identity %.3f s\n", (plat_now_ns() - stage) / 1e9);
    stage = plat_now_ns();
    if (load_cache(&identity, &allocator, &loaded_image, why, why_size)) {
        if (validate_layout(&loaded_image, &identity)) {
            fprintf(stderr, "[gamecode] cache hit image=%zu load=%.3f s\n", loaded_image.size,
                    (plat_now_ns() - stage) / 1e9);
            goto publish;
        }
        gamecode_link_free_metadata(&loaded_image);
        ps5_exec_free(&region);
        memset(&loaded_image, 0, sizeof loaded_image);
        snprintf(why, why_size, "invalid game code layout tables");
    }
    fprintf(stderr, "[gamecode] cache miss: %s\n", why && *why ? why : "not present or invalid");
    if (why && why_size) why[0] = 0;
    size_t hle_size;
    inputs.hle.data = read_file(INCLUDE_ROOT "/hle_symbols.txt", 1024 * 1024, &hle_size);
    inputs.hle.size = inputs.hle.data ? hle_size : 0;
    if (!inputs.hle.data) { snprintf(why, why_size, "game code HLE symbol list is missing"); goto done; }
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    unsigned threads = cores < 1 ? 1 : cores > 8 ? 8 : (unsigned)cores;
    fprintf(stderr, "[gamecode] generating (compiler threads=%u)\n", threads);
    stage = plat_now_ns();
    if (gamecode_build_objects(&inputs, INCLUDE_ROOT, threads, &cache, report_progress, &progress_state,
                               &objects, why, why_size) != 0)
        goto done;
    double seconds = (plat_now_ns() - stage) / 1e9;
    fprintf(stderr, "[gamecode] generate and compile %.3f s objects=%zu compile_throughput=%.2f/s "
            "cache_hits=%zu cache_misses=%zu\n",
            seconds, objects.count, seconds > 0 ? stats.misses / seconds : 0, stats.hits, stats.misses);
    log_heap("compiled");
    free((void *)inputs.hle.data);
    inputs.hle.data = NULL;
    for (size_t i = 0; i < 3; i++) {
        free((void *)inputs.modules[i].data);
        inputs.modules[i].data = NULL;
    }
    if (progress) progress(context, "linking", 0, 1);
    stage = plat_now_ns();
    if (!gamecode_link(objects.objects, objects.count, game_exports, game_export_count, &allocator,
                       &loaded_image, why, why_size)) goto done;
    gamecode_free_objects(&objects);
    fprintf(stderr, "[gamecode] linking %.3f s image=%zu\n", (plat_now_ns() - stage) / 1e9, loaded_image.size);
    log_heap("linked");
    if (!save_cache(&loaded_image, &identity, why, why_size)) goto done;
    fprintf(stderr, "[gamecode] cache saved image=%zu\n", loaded_image.size);
    log_heap("cache saved");
    gamecode_link_free_metadata(&loaded_image);
    ps5_exec_free(&region);
    memset(&loaded_image, 0, sizeof loaded_image);
    gamecode_free_objects(&objects);
    stage = plat_now_ns();
    if (!load_cache(&identity, &allocator, &loaded_image, why, why_size)) {
        if (why && why_size && !*why) snprintf(why, why_size, "saved game code cache could not be reloaded");
        goto done;
    }
    fprintf(stderr, "[gamecode] cache reload image=%zu load=%.3f s\n", loaded_image.size,
            (plat_now_ns() - stage) / 1e9);
publish:
    if (!validate_layout(&loaded_image, &identity)) {
        snprintf(why, why_size, "game code image has invalid or mismatched layout tables");
        goto done;
    }
    fprintf(stderr, "[gamecode] views rx=%p rw=%p bytes=%zu modules=%p module_count=%p version=%p "
            "sites=%p site_count=%p\n", (void *)loaded_image.rx, loaded_image.rw, loaded_image.size,
            loaded_image.game_modules, loaded_image.game_module_count, loaded_image.game_version,
            loaded_image.game_sites, loaded_image.game_site_count);
    ps5_gamelib_set_image(&loaded_image);
    gamecode_link_free_metadata(&loaded_image);
    ok = 1;
done:
    gamecode_free_objects(&objects);
    free((void *)inputs.hle.data);
    for (size_t i = 0; i < 3; i++) free((void *)inputs.modules[i].data);
    if (!ok && region.base) ps5_exec_free(&region);
    if (!ok) gamecode_link_free_metadata(&loaded_image);
    struct ps5_heap_stats heap;
    ps5_heap_stats(&heap);
    fprintf(stderr, "[gamecode] %s total=%.3f s peak_heap=%llu cache_hits=%zu cache_misses=%zu\n",
            ok ? "ready" : "failed", (plat_now_ns() - start) / 1e9,
            (unsigned long long)heap.peak_bytes, stats.hits, stats.misses);
    return ok != 0;
}
