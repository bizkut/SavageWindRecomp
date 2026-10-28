// Enumerate original user-owned merged RomFS Pack/*.pack files, before the game starts.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// The callback borrows path and bytes only until it returns. Return nonzero to stop.
typedef int (*HdUiPackCallback)(void *context, const char *path, const uint8_t *bytes, size_t size);
// fs_game_romfs() must have succeeded first. Returns 0 on success, -1 on read or callback failure.
int fs_hd_ui_packs(HdUiPackCallback callback, void *context);
// Enumerate each Layout/*.sblarc inside those packs, decoding Yaz0 when present.
// Layout names are borrowed and not NUL terminated.
typedef int (*HdUiLayoutCallback)(void *context, const char *pack, const char *layout, size_t layout_length,
                                  const uint8_t *archive, size_t size);
int fs_hd_ui_layouts(HdUiLayoutCallback callback, void *context);
#ifdef __cplusplus
}
#endif
