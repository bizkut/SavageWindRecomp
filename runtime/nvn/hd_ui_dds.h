// Encode generated UI pixels into the runtime's BC4/BC5/BC7 DX10 DDS format.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Input is tightly packed interleaved channels (1, 2 or 4). The caller frees *out.
// Produces the complete mip chain, returns 0 on success.
int hd_ui_dds_encode(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t channels,
                     int srgb, uint8_t **out, size_t *out_size);
// BC7_UNORM, level zero only, for PS5 home-screen art.
int hd_ui_dds_encode_single(const uint8_t *pixels, uint32_t width, uint32_t height,
                            uint8_t **out, size_t *out_size);
#ifdef __cplusplus
}
#endif
