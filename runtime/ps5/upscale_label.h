// PS5 Video page's internal-resolution label for a 16:9 VideoOut mode.
#pragma once
#include <stdio.h>
#include "../nvn/texture_size.h"

static inline void ps5_upscale_label(char *buf, size_t size, const char *name, int w, int h,
                                     double factor, int dynamic_resolution) {
    double fit = fmin(w / 1920.0, h / 1080.0);
    float scale = (float)(fit * (1080.0 / 900.0 / factor));
    unsigned sw = texture_scaled_extent(1600, scale), sh = texture_scaled_extent(900, scale);
    snprintf(buf, size, "%s (%s%u×%u)", name, dynamic_resolution ? "≤" : "", sw, sh);
}
