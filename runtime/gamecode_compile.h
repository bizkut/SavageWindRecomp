#ifndef GAMECODE_COMPILE_H
#define GAMECODE_COMPILE_H

#include <stddef.h>
#include <stdint.h>
#include "gamecode_link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GameCodeBuffer {
    const void *data;
    size_t size;
} GameCodeBuffer;

typedef struct GameCodeCompileInputs {
    GameCodeBuffer modules[3]; /* main, subsdk0, sdk, in load order */
    GameCodeBuffer layout; /* optional; when supplied it must match the NSOs */
    GameCodeBuffer hle;
    uint8_t llvm_build_identity[32]; /* SHA-256 of the compiler build, including patches. */
} GameCodeCompileInputs;

typedef struct GameCodeObjectCache {
    void *context;
    /* 1: hit, 0: miss, -1: error. A hit remains valid until release. */
    int (*lookup)(void *context, const uint8_t key[32], const void **data, size_t *size);
    void (*release)(void *context, const void *data, size_t size);
    /* Return zero on success. Data remains valid only for the duration of the call. */
    int (*store)(void *context, const uint8_t key[32], const void *data, size_t size);
} GameCodeObjectCache;

typedef void (*GameCodeProgress)(void *context, const char *stage, size_t done, size_t total);

typedef struct GameCodeObjects {
    GameLinkObject *objects;
    size_t count;
    void *private_data;
} GameCodeObjects;

/* include_root contains runtime/, include/, and clang-include/. Inputs are borrowed.
 * Cache and progress callbacks are serialized, including across worker threads.
 * On success the returned objects are owned until gamecode_free_objects(). */
int gamecode_build_objects(const GameCodeCompileInputs *inputs, const char *include_root, unsigned threads,
                           const GameCodeObjectCache *cache, GameCodeProgress progress, void *progress_context,
                           GameCodeObjects *out, char *error, size_t error_size);
void gamecode_free_objects(GameCodeObjects *objects);

#ifdef __cplusplus
}
#endif
#endif
