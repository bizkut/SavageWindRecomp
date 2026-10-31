#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t code;
    int16_t left;
    uint16_t width, advance;
} PlayerFontGlyph;
typedef struct {
    double weight, width_axis, baseline_shift_limit;
} PlayerFontPreset;


typedef struct {
    double scale, baseline, embolden, weight, width_axis;
    double cap_height, x_height, scale_factor, min_glyph_factor, baseline_shift;
    unsigned fallback, fitted, condensed, scaled;
} PlayerFontFit;

bool player_font_render(const char *path, const PlayerFontGlyph *glyphs, size_t count, int w, int h, int cw, int ch,
                        int cols, int rows, int scale, const PlayerFontFit *fit, uint8_t *output);
bool player_font_fit(const char *path, const PlayerFontGlyph *glyphs, size_t count, const uint8_t *source,
                     uint8_t *output, int w, int h, int cw, int ch, int cols, int rows, int scale,
                     double baseline, const PlayerFontPreset *preset, PlayerFontFit *fit);
