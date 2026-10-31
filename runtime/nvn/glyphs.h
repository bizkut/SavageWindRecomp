// Button icons for the device in use (runtime/nvn/glyphs.c): the game's button glyph font sheet, redrawn.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The identified guest sheet is decoded from linear BC4 blocks. scale 2 or 4 returns a malloc'd R8 sheet in *high;
// scale 1 modifies lin in place. The caller frees *high. Unchanged cells retain the game's original texels.
bool glyphs_apply(uint64_t addr, uint64_t pool_lo, uint64_t pool_hi, uint32_t format, uint32_t w, uint32_t h,
                  uint8_t *lin, uint32_t wb, uint32_t hb, int scale, uint8_t **high);

// Layout button masks take precedence over artwork upscalers.
bool glyphs_layout_match(uint32_t format, uint32_t w, uint32_t h, const uint8_t *lin, uint32_t row);

// The same for the HUD's R8 textures; high-resolution output is tightly packed.
bool glyphs_apply_layout(uint32_t format, uint32_t w, uint32_t h, uint8_t *lin, uint32_t row, int scale, uint8_t **high);

// Register text sheets and cell geometry parsed from one user-supplied Switch BFFNT.
void glyphs_register_font(const uint8_t *data, uint64_t size);

// Recognized sheets use a per-session worker cache; returned high data is borrowed, not freed by the caller.
bool glyphs_sharp_text(uint64_t addr, uint64_t pool_lo, uint64_t pool_hi, uint32_t format,
                       uint32_t w, uint32_t h, const uint8_t *lin, int scale, bool log_upload,
                       uint8_t **high, int *pending);
bool glyphs_text_ready(int pending);

// Changes whenever the icons to draw change (device, bindings, setting): sheets uploaded under another one go up again.
uint32_t glyphs_generation(void);

// A line of text in Fira Sans SemiBold (the labels' font), its capitals cap_h pixels tall (smaller when it would be
// wider than max_w): coverage 0..255, *w x *h bytes in rows, malloc'd (free it). NULL when the font is missing.
uint8_t *glyphs_text_line(const char *s, float cap_h, float max_w, int *w, int *h);
