#ifndef GAMECODE_CACHE_H
#define GAMECODE_CACHE_H

#include "gamecode_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAMECODE_CACHE_MODULES 3
#define GAMECODE_CACHE_LLVM_VERSION_SIZE 128
/* Increment the linker version whenever image layout or fixup semantics change. */
#define GAMECODE_LINKER_VERSION 1u
#define GAMECODE_CACHE_FORMAT_VERSION 2u
#define GAMECODE_CACHE_MANIFEST_SIZE 752u

typedef struct GameCodeIdentity {
    unsigned char module_build_ids[GAMECODE_CACHE_MODULES][32];
    unsigned char module_sha256[GAMECODE_CACHE_MODULES][32];
    unsigned char generator_version[32];
    char llvm_version[GAMECODE_CACHE_LLVM_VERSION_SIZE];
    unsigned char compile_flags_sha256[32];
    unsigned char export_abi_sha256[32];
    unsigned char llvm_build_sha256[32];
    uint64_t linker_version;
    uint64_t cache_format_version;
} GameCodeIdentity;

/* Callbacks return nonzero only after transferring the entire requested range.
 * The caller is responsible for atomic publication of the completed cache. */
typedef struct GameCodeCacheWriter {
    void *context;
    int (*write)(void *context, uint64_t offset, const void *data, size_t size);
} GameCodeCacheWriter;

typedef struct GameCodeCacheReader {
    void *context;
    uint64_t size;
    int (*read)(void *context, uint64_t offset, void *data, size_t size);
} GameCodeCacheReader;

int gamecode_cache_save(const GameLinkImage *image, const GameCodeIdentity *identity,
                        const GameCodeCacheWriter *writer, char *error, size_t error_size);
int gamecode_cache_load(const GameCodeIdentity *identity, const GameCodeCacheReader *reader,
                        const GameLinkAllocator *allocator, const GameExport *exports, size_t export_count,
                        GameLinkImage *image, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif
#endif
