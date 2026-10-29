// Shared PC/console UI-pack archive enumeration and texture selection.
#pragma once
#include <stddef.h>
#include <stdint.h>
#define HD_UI_GENERATOR_VERSION 3  // (3: item icons, UI/StockItem/*.sbitemico, join the layouts)
#ifdef __cplusplus
extern "C" {
#endif
// Enumerate named SARC entries. Returns 1 for an entry, 0 at end, -1 for invalid input.
// Returned name and payload pointers borrow the archive buffer.
int hd_ui_sarc_entry(const uint8_t *data, size_t size, uint32_t index,
                     const char **name, size_t *name_len, const uint8_t **payload, size_t *payload_size);
// Enumerate Layout/*.sblarc entries, advancing *cursor; same return and pointer lifetime.
int hd_ui_sarc_layout(const uint8_t *data, size_t size, uint32_t *cursor,
                      const char **name, size_t *name_len, const uint8_t **payload, size_t *payload_size);
// 0 keep, 1 smaller than 16, 2 exceeds max_size, 3 flat/gradient/low-energy mask.
// Pixels are interleaved, one to four 8-bit channels, stride in bytes.
int hd_ui_texture_policy(uint32_t width, uint32_t height, uint32_t max_size,
                         uint32_t channels, const uint8_t *pixels, size_t stride);
uint32_t hd_ui_texture_scale(uint32_t width, uint32_t height);
uint32_t hd_ui_generator_version(void);
// BNTX format to block dimensions, bytes per block, output channels and NVN format.
// Returns zero for unsupported formats.
int hd_ui_source_format(uint32_t bntx_format, uint32_t *block, uint32_t *bpb,
                        uint32_t *channels, uint32_t *nvn_format);
#ifdef __cplusplus
}
#endif
