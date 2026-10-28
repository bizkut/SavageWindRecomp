// Pixel extent of a scaled render target, shared with the PS5 Video page.
#pragma once
#include <math.h>
#include <stdint.h>

static inline uint32_t texture_scaled_extent(uint32_t extent, float scale) {
    uint32_t size = (uint32_t)lroundf(extent * scale);
    return size < 1 ? 1 : size;
}
