// Run after ps5/tools/setup-opensans.sh; link with player_font.c and FreeType as in test_font_fit.c.
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "../runtime/nvn/player_font.h"


int main(void) {
    enum { CW = 24, CH = 30, COLS = 6, W = 150, H = 32, S = 4 };
    const PlayerFontGlyph glyphs[COLS] = {
        {'A', 0, 18, 18}, {'B', 0, 18, 18}, {'g', 0, 17, 17},
        {'o', 0, 17, 17}, {'W', 0, 8, 8}, {0x10ffff, 0, 18, 18}
    };
    uint8_t source[W * H] = {0};
    uint8_t high[W * H * S * S] = {0};
    uint8_t result[sizeof high];
    PlayerFontFit expected = {.scale = 20, .baseline = 23, .weight = 700, .width_axis = 85};
    const char *font = ".deps/ps5/opensans/OpenSans.ttf";
    // A narrow advance may condense horizontally, but must not change an individual glyph's height.
    const PlayerFontGlyph pair[2] = {{'A', 0, 20, 20}, {'A', 0, 7, 7}};
    uint8_t pair_pixels[sizeof high] = {0};
    assert(player_font_render(font, pair, 2, W, H, CW, CH, COLS, 1, S, &expected, pair_pixels));
    int heights[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        int first = CH * S, last = -1;
        for (int y = 0; y < CH * S; y++) for (int x = 0; x < CW * S; x++)
            if (pair_pixels[(y + 2 * S) * W * S + i * (CW + 1) * S + x]) {
                if (y < first) first = y;
                if (y > last) last = y;
            }
        heights[i] = last - first + 1;
    }
    assert(heights[0] > 0 && heights[0] == heights[1]);
    assert(player_font_render(font, glyphs, COLS, W, H, CW, CH, COLS, 1, S, &expected, high));
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int sum = 0;
        for (int j = 0; j < S; j++) for (int i = 0; i < S; i++)
            sum += high[(y * S + j) * W * S + x * S + i];
        source[y * W + x] = (uint8_t)((sum + 8) / 16);
    }
    // A different source shape must not cause a supported glyph to fall back.
    memset(source + 4 * (CW + 1), 0, CW);
    memset(result, 37, sizeof result);
    PlayerFontFit fit;
    const PlayerFontPreset preset = {.weight = 612.5, .width_axis = 100, .baseline_shift_limit = 1};
    assert(player_font_fit(font, glyphs, COLS, source, result, W, H, CW, CH, COLS, 1, S, 23, &preset, &fit));
    assert(fabs(fit.scale - expected.scale) < 2);
    assert(fit.baseline == 23);
    assert(fit.width_axis == 100 && fit.weight == 612.5);
    assert(fit.scale_factor > 0 && fit.scale_factor <= 1);
    assert(fit.fitted == 5 && fit.fallback == 1);
    for (int i = 0; i < 5; i++) for (int y = 0; y < CH * S; y++) for (int x = 0; x < CW * S; x++) {
        uint8_t v = result[(y + 2 * S) * W * S + i * (CW + 1) * S + x];
        if (x < 2 || y < 2 || x >= CW * S - 2 || y >= CH * S - 2) assert(v == 0);
    }
    // Each CWDH pen interval retains a full original-pixel blank column between letters.
    int pen = 0;
    int previous_right = -1000;
    for (int i = 0; i < 5; i++) {
        int left = W * S, right = -1;
        for (int y = 2 * S; y < (2 + CH) * S; y++) for (int x = 0; x < CW * S; x++) {
            if (!result[y * W * S + i * (CW + 1) * S + x]) continue;
            int at = pen + glyphs[i].left * S + x;
            if (at < left) left = at;
            if (at > right) right = at;
        }
        assert(right >= 0);
        assert(left - previous_right >= S);
        previous_right = right;
        pen += glyphs[i].advance * S;
    }
    assert(fit.condensed >= 1);
    for (int y = 0; y < CH * S; y++) for (int x = 0; x < CW * S; x++) {
        int guard = x < 2 || y < 2 || x >= CW * S - 2 || y >= CH * S - 2;
        assert(result[(y + 2 * S) * W * S + 5 * (CW + 1) * S + x] == (guard ? 0 : 37));
    }
    // An unusually tall non-ASCII glyph may shrink, but it must not shrink the ASCII sheet scale.
    const PlayerFontGlyph accents[3] = {{'A', 0, 18, 18}, {'B', 0, 18, 18}, {0x01fa, 0, 17, 17}};
    uint8_t accented[sizeof high] = {0};
    PlayerFontFit accent_fit;
    assert(player_font_fit(font, accents, 3, source, accented, W, H, CW, CH, COLS, 1, S, 15, &preset, &accent_fit));
    assert(accent_fit.fitted == 3 && accent_fit.fallback == 0);
    assert(accent_fit.scaled >= 1 && accent_fit.min_glyph_factor > 0 && accent_fit.min_glyph_factor < 1);
    // A constrained descender uses up to one source pixel of available headroom before reducing sheet scale.
    uint8_t shifted[sizeof high] = {0};
    PlayerFontFit shift_fit;
    assert(player_font_fit(font, glyphs, COLS, source, shifted, W, H, CW, CH, COLS, 1, S, 26, &preset, &shift_fit));
    assert(shift_fit.baseline_shift > 0 && shift_fit.baseline_shift <= 1);
    assert(shift_fit.fitted == 5 && shift_fit.fallback == 1);
    return 0;
}
