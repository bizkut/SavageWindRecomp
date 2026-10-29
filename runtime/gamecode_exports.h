// Runtime addresses available to separately linked game code.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GameExport {
    const char *name;
    void *address;
} GameExport;

extern const GameExport game_exports[];
extern const size_t game_export_count;
extern const char game_export_abi_digest[];

#ifdef __cplusplus
}
#endif
