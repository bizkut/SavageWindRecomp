#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Index selected named textures from a user-supplied BNTX, never the whole atlas.
void sharp_ui_register(const uint8_t *data, size_t size);
// Match a linear mip-zero upload and schedule its immutable RGBA replacement.
bool sharp_ui_candidate(uint32_t format, uint32_t w, uint32_t h);
bool sharp_ui_request(uint32_t format, uint32_t w, uint32_t h, const uint8_t *linear,
                      uint8_t **rgba, int *pending);
bool sharp_ui_ready(int pending);

typedef struct {
    uint8_t *data;
    size_t bytes;
    uint32_t width, height, levels, dxgi;
    uint64_t hash;
} HdUiImage;
bool hd_ui_available(void);
bool hd_ui_load(const uint8_t *linear, size_t bytes, uint32_t width, uint32_t height, uint32_t format, HdUiImage *out);
void hd_ui_note_rejected(const uint8_t *linear, size_t bytes, uint32_t width, uint32_t height,
                         uint32_t format, bool storage, bool scalable, uint32_t samples, uint32_t level_mask);
// Diagnostic manifest candidates for a guest upload; does not alter matching.
bool hd_ui_diag_candidate(uint32_t width, uint32_t height);
void hd_ui_diag_upload(uint32_t width, uint32_t height, uint32_t format, uint32_t levels,
                       uint32_t layers, uint32_t layer, uint32_t target, bool pitch,
                       bool gpu_owned, bool eligible, const uint8_t *linear, size_t bytes);
