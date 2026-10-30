static int cache_write(void *context, uint64_t offset, const void *data, size_t size)
{
    return pwrite(*(int *)context, data, size, (off_t)offset) == (ssize_t)size;
}

static int cache_read(void *context, uint64_t offset, void *data, size_t size)
{
    return pread(*(int *)context, data, size, (off_t)offset) == (ssize_t)size;
}

static double elapsed(struct timespec a, struct timespec b)
{
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

static void cache_test(GameLinkObject *objects, size_t count, GameExport *exports, size_t export_count,
                       GameLinkAllocator *allocator, GameLinkImage *initial)
{
    GameCodeIdentity identity = {0};
    memcpy(identity.llvm_version, "clang-18", 9);
    identity.linker_version = GAMECODE_LINKER_VERSION;
    identity.cache_format_version = GAMECODE_CACHE_FORMAT_VERSION;
    char cache_path[] = "build/gamecode-cache.XXXXXX";
    int fd = mkstemp(cache_path);
    CHECK(fd >= 0);
    GameCodeCacheWriter writer = {&fd, cache_write};
    char error[256];
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    CHECK(gamecode_cache_save(initial, &identity, &writer, error, sizeof error));
    clock_gettime(CLOCK_MONOTONIC, &end);
    double save_seconds = elapsed(start, end);
    struct stat st;
    CHECK(!fstat(fd, &st));
    GameCodeCacheReader reader = {&fd, (uint64_t)st.st_size, cache_read};
    size_t image_size = initial->size;
    gamecode_link_free_metadata(initial);

    int backing = memfd_create("gamecode-cache-second", 0);
    Buffer second = {0};
    second.capacity = ((Buffer *)allocator->context)->capacity;
    CHECK(backing >= 0 && !ftruncate(backing, second.capacity));
    second.memory = mmap(NULL, second.capacity, PROT_READ | PROT_WRITE, MAP_SHARED, backing, 0);
    second.rx = (uintptr_t)mmap(NULL, second.capacity, PROT_READ | PROT_EXEC, MAP_SHARED, backing, 0);
    close(backing);
    CHECK(second.memory != MAP_FAILED && second.rx != (uintptr_t)MAP_FAILED);
    GameLinkAllocator other = {&second, allocate_image, release_image};
    double load_seconds = 0;
    for (int pass = 0; pass < 2; pass++) {
        GameLinkAllocator *a = pass ? &other : allocator;
        GameLinkImage loaded, fresh;
        clock_gettime(CLOCK_MONOTONIC, &start);
        CHECK(gamecode_cache_load(&identity, &reader, a, exports, export_count, &loaded, error, sizeof error));
        clock_gettime(CLOCK_MONOTONIC, &end);
        load_seconds += elapsed(start, end);
        CHECK(loaded.size == image_size);
        unsigned char *snapshot = malloc(image_size);
        CHECK(snapshot);
        memcpy(snapshot, loaded.rw, image_size);
        void *layout[] = {loaded.game_modules, loaded.game_module_count, loaded.game_version,
                          loaded.game_sites, loaded.game_site_count};
        CHECK(gamecode_link(objects, count, exports, export_count, a, &fresh, error, sizeof error));
        CHECK(!memcmp(snapshot, fresh.rw, image_size));
        CHECK(layout[0] == fresh.game_modules && layout[1] == fresh.game_module_count &&
              layout[2] == fresh.game_version && layout[3] == fresh.game_sites && layout[4] == fresh.game_site_count);
        free(snapshot);
        gamecode_link_free_metadata(&loaded);
        gamecode_link_free_metadata(&fresh);
    }
    GameLinkImage rejected;
    GameCodeCacheReader truncated = reader;
    truncated.size--;
    CHECK(!gamecode_cache_load(&identity, &truncated, allocator, exports, export_count, &rejected, error,
                               sizeof error));
    uint64_t offsets[] = {0, GAMECODE_CACHE_MANIFEST_SIZE + image_size / 2,
                          GAMECODE_CACHE_MANIFEST_SIZE + image_size};
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        unsigned char byte;
        CHECK(cache_read(&fd, offsets[i], &byte, 1));
        byte ^= 1;
        CHECK(cache_write(&fd, offsets[i], &byte, 1));
        CHECK(!gamecode_cache_load(&identity, &reader, allocator, exports, export_count, &rejected, error,
                                   sizeof error));
        byte ^= 1;
        CHECK(cache_write(&fd, offsets[i], &byte, 1));
    }
    unsigned char *id = (unsigned char *)&identity;
    for (size_t i = 0; i < sizeof identity; i++) {
        id[i] ^= 1;
        CHECK(!gamecode_cache_load(&identity, &reader, allocator, exports, export_count, &rejected, error,
                                   sizeof error));
        id[i] ^= 1;
    }
    size_t fixup_size = (size_t)reader.size - GAMECODE_CACHE_MANIFEST_SIZE - image_size;
    CHECK(fixup_size >= 48);
    unsigned char manifest[GAMECODE_CACHE_MANIFEST_SIZE];
    unsigned char *fixups = malloc(fixup_size);
    CHECK(fixups && cache_read(&fd, 0, manifest, sizeof manifest) &&
          cache_read(&fd, GAMECODE_CACHE_MANIFEST_SIZE + image_size, fixups, fixup_size));
    unsigned char original_type[4];
    memcpy(original_type, fixups + 36, sizeof original_type);
    fixups[36] = 0xff;
    fixups[37] = 0xff;
    fixups[38] = 0xff;
    fixups[39] = 0xff;
    CHECK(!mbedtls_sha256_ret(fixups, fixup_size, manifest + GAMECODE_CACHE_MANIFEST_SIZE - 64, 0));
    CHECK(!mbedtls_sha256_ret(manifest, GAMECODE_CACHE_MANIFEST_SIZE - 32,
                              manifest + GAMECODE_CACHE_MANIFEST_SIZE - 32, 0));
    CHECK(cache_write(&fd, 0, manifest, sizeof manifest) &&
          cache_write(&fd, GAMECODE_CACHE_MANIFEST_SIZE + image_size, fixups, fixup_size));
    CHECK(!gamecode_cache_load(&identity, &reader, allocator, exports, export_count, &rejected, error, sizeof error));
    memcpy(fixups + 36, original_type, sizeof original_type);
    CHECK(!mbedtls_sha256_ret(fixups, fixup_size, manifest + GAMECODE_CACHE_MANIFEST_SIZE - 64, 0));
    CHECK(!mbedtls_sha256_ret(manifest, GAMECODE_CACHE_MANIFEST_SIZE - 32,
                              manifest + GAMECODE_CACHE_MANIFEST_SIZE - 32, 0));
    CHECK(cache_write(&fd, 0, manifest, sizeof manifest) &&
          cache_write(&fd, GAMECODE_CACHE_MANIFEST_SIZE + image_size, fixups, fixup_size));
    free(fixups);
    close(fd);
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    printf("cache=%s cache_bytes=%jd save_seconds=%.3f load_seconds_total=%.3f peak_rss_kib=%ld\n",
           cache_path, (intmax_t)st.st_size, save_seconds, load_seconds, usage.ru_maxrss);
    munmap((void *)second.rx, second.capacity);
    munmap(second.memory, second.capacity);
}
