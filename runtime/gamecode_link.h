#ifndef GAMECODE_LINK_H
#define GAMECODE_LINK_H

#include <stddef.h>
#include <stdint.h>
#include "gamecode_exports.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GameLinkObject {
    const void *data;
    size_t size;
    const char *name;
} GameLinkObject;

/* The views must map the same bytes and satisfy alignment; they remain owned by the caller.
 * release is called on failure after allocation, never on success. */
typedef struct GameLinkAllocator {
    void *context;
    int (*allocate)(void *context, size_t size, size_t alignment, void **rw, uintptr_t *rx);
    void (*release)(void *context, void *rw, size_t size);
} GameLinkAllocator;

typedef struct GameLinkImage {
    void *rw;
    uintptr_t rx;
    size_t size;
    size_t text_size, rodata_size, data_size, bss_size;
    size_t got_count, thunk_count;
    void *game_modules, *game_module_count, *game_version, *game_sites, *game_site_count;
} GameLinkImage;

/* Exports must be sorted by name. On failure error receives a diagnostic if provided. */
int gamecode_link(const GameLinkObject *objects, size_t count, const GameExport *exports,
                  size_t export_count, const GameLinkAllocator *allocator, GameLinkImage *image,
                  char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
