// Run after ps5/tools/setup-opensans.sh.
// cc -std=c11 -O2 -DPLAYER_FONT_TEST $(pkg-config --cflags freetype2) tools/test_font_fit.c \
//     runtime/nvn/player_font.c $(pkg-config --libs freetype2) -lm -o /tmp/test_font_fit && /tmp/test_font_fit
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/nvn/player_font.h"

int main(void) {
    enum { CW = 24, CH = 30, COLS = 4, ROWS = 1, W = COLS * (CW + 1), H = 32, S = 4 };
    const PlayerFontGlyph glyphs[COLS] = {{'A', 0, 20, 20}, {'B', 0, 20, 20}, {'g', 0, 20, 20},
                                          {0x10ffff, 0, 20, 20}};
    uint8_t source[W * H] = {0};
    uint8_t high[W * H * S * S] = {0};
    uint8_t result[sizeof high];
    PlayerFontFit expected = {.scale = 20.0, .baseline = 23.0};
    assert(player_font_render(".deps/ps5/opensans/OpenSans.ttf", glyphs, COLS, W, H, CW, CH, COLS, ROWS,
                              S, &expected, high));
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int sum = 0;
        for (int j = 0; j < S; j++) for (int i = 0; i < S; i++)
            sum += high[(y * S + j) * W * S + x * S + i];
        source[y * W + x] = (uint8_t)((sum + 8) / 16);
    }
    memset(result, 37, sizeof result);
    PlayerFontFit fit;
    assert(!player_font_fit("/nonexistent/OpenSans.ttf", glyphs, COLS, source, result, W, H,
                            CW, CH, COLS, ROWS, S, 23, NULL, &fit));
    for (size_t i = 0; i < sizeof result; i++) assert(result[i] == 37);
    assert(player_font_fit(".deps/ps5/opensans/OpenSans.ttf", glyphs, COLS, source, result, W, H,
                           CW, CH, COLS, ROWS, S, 23, NULL, &fit));
    assert(fabs(fit.scale - expected.scale) < 1.0);
    assert(fabs(fit.baseline - expected.baseline) < 1.0);
    assert(fit.fitted == 3 && fit.fallback == 1);
    for (int y = 0; y < CH * S; y++) for (int x = 0; x < CW * S; x++) {
        int guard = x < 2 || y < 2 || x >= CW * S - 2 || y >= CH * S - 2;
        assert(result[(y + 2 * S) * W * S + 3 * (CW + 1) * S + x] == (guard ? 0 : 37));
    }
    assert(fit.fallback >= 1);
    puts("font synthetic fit passed");
    return 0;
}
