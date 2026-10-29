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

typedef enum GameLinkTargetKind {
    GAME_LINK_TARGET_NONE = 0,
    GAME_LINK_TARGET_RW = 1,
    GAME_LINK_TARGET_RX = 2,
    GAME_LINK_TARGET_EXPORT = 3,
    GAME_LINK_TARGET_ABSOLUTE = 4
} GameLinkTargetKind;

typedef struct GameLinkTarget {
    uint64_t value;
    uint32_t kind;
} GameLinkTarget;

/* slot_offset is UINT64_MAX for PC32 and 64, otherwise a reserved GOT or thunk offset.
 * place_view is GAME_LINK_TARGET_RW or GAME_LINK_TARGET_RX. */
typedef struct GameLinkFixup {
    uint64_t offset;
    int64_t addend;
    uint64_t slot_offset;
    GameLinkTarget target;
    uint32_t type;
    uint32_t place_view;
} GameLinkFixup;

typedef struct GameLinkImage {
    void *rw;
    uintptr_t rx;
    size_t size;
    size_t text_size, rodata_size, data_size, bss_size;
    size_t alignment;
    unsigned char *canonical;
    GameLinkFixup *fixups;
    size_t fixup_count;
    /* modules, module_count, version, sites, site_count; NONE means absent. */
    GameLinkTarget layout[5];
    size_t got_offset, thunk_offset, thunk_capacity;
    size_t got_count, thunk_count;
    void *game_modules, *game_module_count, *game_version, *game_sites, *game_site_count;
} GameLinkImage;

/* Exports must be sorted by name. On failure error receives a diagnostic if provided. */
int gamecode_link(const GameLinkObject *objects, size_t count, const GameExport *exports,
                  size_t export_count, const GameLinkAllocator *allocator, GameLinkImage *image,
                  char *error, size_t error_size);

/* Copies canonical bytes to a distinct rw buffer before applying; reserved thunk choices are
 * recomputed at each base. On failure rw may be partially relocated and must not be executed.
 * Exports must have the same ordered ABI identity used when the image was linked. The live
 * pointer fields and thunk_count describe the original link; use layout[] to resolve new views. */
int gamecode_link_apply_fixups(const GameLinkImage *image, void *rw, uintptr_t rx,
                               const GameExport *exports, size_t count, char *error, size_t error_size);

/* Frees canonical and fixups, not the caller-owned execution views. */
void gamecode_link_free_metadata(GameLinkImage *image);

#ifdef __cplusplus
}
#endif

#endif
