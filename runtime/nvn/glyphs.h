// Button icons for the device in use (runtime/nvn/glyphs.c): the game's button glyph font sheet, redrawn.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The texture at addr (format: NVN format, w x h texels) is a button glyph sheet: lin holds its texels as uploaded
// (linear rows of 4x4 BC4 blocks, wb x hb); they are replaced by the glyphs for the current device. Returns true when
// the texture is such a sheet (whether or not it changed it).
bool glyphs_apply(uint64_t addr, uint64_t pool_lo, uint64_t pool_hi, uint32_t format, uint32_t w, uint32_t h,
                  uint8_t *lin, uint32_t wb, uint32_t hb);

// The same for the HUD's button textures (R8 masks, w x h texels, rows `row` bytes apart in lin): true when the texture
// is one of them.
bool glyphs_apply_layout(uint32_t format, uint32_t w, uint32_t h, uint8_t *lin, uint32_t row);

// Changes whenever the icons to draw change (device, bindings, setting): sheets uploaded under another one go up again.
uint32_t glyphs_generation(void);

// A line of text in Fira Sans SemiBold (the labels' font), its capitals cap_h pixels tall (smaller when it would be
// wider than max_w): coverage 0..255, *w x *h bytes in rows, malloc'd (free it). NULL when the font is missing.
uint8_t *glyphs_text_line(const char *s, float cap_h, float max_w, int *w, int *h);
