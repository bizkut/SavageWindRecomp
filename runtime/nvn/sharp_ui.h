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
