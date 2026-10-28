// Decode the user-owned layout texture inputs used by the HD UI generator.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    size_t name_length;
    uint32_t format, width, height, channels, scale;
    uint64_t hash;
    const uint8_t *linear; // original deswizzled level 0
    size_t linear_size;
    const uint8_t *pixels; // interleaved 8-bit source channels
} HdUiTexture;
typedef int (*HdUiTextureCallback)(void *context, const HdUiTexture *texture);
// Enumerates supported, selected BNTX textures from one decompressed layout SARC.
// Borrowed data remains valid only for the callback. Returns -1 on malformed data or callback failure.
int hd_ui_extract_layout(const uint8_t *archive, size_t size, HdUiTextureCallback callback, void *context);
#ifdef __cplusplus
}
#endif
