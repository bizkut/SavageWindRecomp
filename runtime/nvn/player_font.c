#if defined(__PROSPERO__) || defined(PLAYER_FONT_TEST)
#include "player_font.h"
#include "text_reconstruct.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_MULTIPLE_MASTERS_H
#include <math.h>
#include <stdlib.h>
#include <string.h>


static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median(double *values, size_t count) {
    if (!count) return 0;
    qsort(values, count, sizeof *values, compare_double);
    return count & 1 ? values[count / 2] : (values[count / 2 - 1] + values[count / 2]) / 2;
}

static bool axis_info(FT_Library library, FT_Face face, FT_ULong tag,
                      double *minimum, double *maximum, double *initial) {
    FT_MM_Var *axes = NULL;
    if (FT_Get_MM_Var(face, &axes)) return false;
    bool found = false;
    for (FT_UInt i = 0; i < axes->num_axis; i++) {
        if (axes->axis[i].tag != tag) continue;
        *minimum = axes->axis[i].minimum / 65536.0;
        *maximum = axes->axis[i].maximum / 65536.0;
        *initial = axes->axis[i].def / 65536.0;
        found = true;
        break;
    }
    FT_Done_MM_Var(library, axes);
    return found;
}

static bool prepare_face(FT_Library library, FT_Face face, const PlayerFontFit *fit, int scale) {
    if (fit->weight || fit->width_axis) {
        FT_MM_Var *axes = NULL;
        if (FT_Get_MM_Var(face, &axes)) return false;
        FT_Fixed *coordinates = malloc(axes->num_axis * sizeof *coordinates);
        if (!coordinates) {
            FT_Done_MM_Var(library, axes);
            return false;
        }
        for (FT_UInt i = 0; i < axes->num_axis; i++) {
            coordinates[i] = axes->axis[i].def;
            if (fit->weight && axes->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't'))
                coordinates[i] = (FT_Fixed)lround(fit->weight * 65536);
            if (fit->width_axis && axes->axis[i].tag == FT_MAKE_TAG('w', 'd', 't', 'h'))
                coordinates[i] = (FT_Fixed)lround(fit->width_axis * 65536);
        }
        int rc = FT_Set_Var_Design_Coordinates(face, axes->num_axis, coordinates);
        free(coordinates);
        FT_Done_MM_Var(library, axes);
        if (rc) return false;
    }
    FT_Set_Transform(face, NULL, NULL);
    return FT_Set_Char_Size(face, 0, (FT_F26Dot6)lround(fit->scale * scale * 64), 72, 72) == 0;
}

// Reserve one game pixel between neighbouring ink boxes without changing the game's advances.
static void glyph_bounds(const PlayerFontGlyph *glyph, int cw, double *left, double *right) {
    *left = 0;
    *right = glyph->width ? glyph->width : cw;
    if (*right > cw) *right = cw;
    if (glyph->advance) {
        double lo = 0.5 - glyph->left;
        double hi = glyph->advance - 0.5 - glyph->left;
        if (lo > *left) *left = lo;
        if (hi < *right) *right = hi;
    }
    if (*right <= *left) {
        *left = 0;
        *right = glyph->width && glyph->width < cw ? glyph->width : cw;
    }
}

// Keep the entire outline inside its cell and pen interval, preserving a two-texel atlas guard.
static bool draw(FT_Face face, const PlayerFontGlyph *glyph, int cw, int ch, int scale,
                 const PlayerFontFit *fit, uint8_t *cell, bool *condensed, double *glyph_factor) {
    memset(cell, 0, (size_t)cw * ch * scale * scale);
    *condensed = false;
    if (glyph_factor) *glyph_factor = 1;
    FT_UInt index = FT_Get_Char_Index(face, glyph->code);
    if (!index || FT_Load_Glyph(face, index, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) ||
        face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return false;
    FT_Outline *outline = &face->glyph->outline;
    if (fit->embolden && FT_Outline_Embolden(outline, (FT_Pos)lround(fit->embolden * scale * 64))) return false;
    FT_BBox box;
    FT_Outline_Get_CBox(outline, &box);
    double left, right;
    glyph_bounds(glyph, cw, &left, &right);
    left *= scale;
    right *= scale;
    if (left < 3) left = 3;
    if (right > cw * scale - 3) right = cw * scale - 3;
    if (right <= left) return false;
    double target = right - left;
    double width = (box.xMax - box.xMin) / 64.0;
    if (width > target && width > 0) {
        double factor = target / width;
        FT_Matrix matrix = {(FT_Fixed)floor(factor * 65536), 0, 0, 65536};
        FT_Outline_Transform(outline, &matrix);
        FT_Outline_Get_CBox(outline, &box);
        *condensed = true;
    }
    // Preserve ASCII's shared scale. Only an oversized non-ASCII outline may shrink vertically.
    if (glyph->code >= 0x80) {
        double factor = 1;
        double top_room = fit->baseline * scale - 3;
        double bottom_room = (ch - fit->baseline) * scale - 3;
        if (top_room <= 0 || bottom_room <= 0) return false;
        if (box.yMax > 0 && box.yMax / 64.0 > top_room) factor = top_room / (box.yMax / 64.0);
        if (box.yMin < 0 && -box.yMin / 64.0 > bottom_room) {
            double q = bottom_room / (-box.yMin / 64.0);
            if (q < factor) factor = q;
        }
        if (factor < 1) {
            FT_Fixed shrink = (FT_Fixed)floor(factor * 65536);
            FT_Matrix matrix = {shrink, 0, 0, shrink};
            FT_Outline_Transform(outline, &matrix);
            FT_Outline_Get_CBox(outline, &box);
            if (glyph_factor) *glyph_factor = shrink / 65536.0;
        }
    }
    FT_Pos shift = (FT_Pos)lround(left * 64 + (target * 64 - (box.xMax - box.xMin)) / 2) - box.xMin;
    FT_Outline_Translate(outline, shift, 0);
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) return false;
    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap *bm = &slot->bitmap;
    if (bm->pixel_mode != FT_PIXEL_MODE_GRAY && bm->width && bm->rows) return false;
    int width_pixels = cw * scale, height_pixels = ch * scale;
    int top = (int)lround(fit->baseline * scale) - slot->bitmap_top;
    for (unsigned y = 0; y < bm->rows; y++) for (unsigned x = 0; x < bm->width; x++) {
        int dx = slot->bitmap_left + (int)x, dy = top + (int)y;
        const uint8_t *row = bm->pitch >= 0 ? bm->buffer + y * bm->pitch :
                             bm->buffer + (bm->rows - 1 - y) * -bm->pitch;
        if (dx < 2 || dx >= width_pixels - 2 || dy < 2 || dy >= height_pixels - 2) {
            if (row[x]) return false;
            continue;
        }
        cell[(height_pixels - 1 - dy) * width_pixels + dx] = row[x];
    }
    return true;
}
// Reconstruct the first build's tone histogram without publishing its unguarded cell pixels.
static bool draw_v1(FT_Face face, const PlayerFontGlyph *glyph, int cw, int ch, int scale,
                 const PlayerFontFit *fit, uint8_t *cell, bool *condensed, bool *scaled) {
    memset(cell, 0, (size_t)cw * ch * scale * scale);
    *condensed = false;
    if (scaled) *scaled = false;
    FT_UInt index = FT_Get_Char_Index(face, glyph->code);
    if (!index || FT_Load_Glyph(face, index, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) ||
        face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return false;
    FT_Outline *outline = &face->glyph->outline;
    if (fit->embolden && FT_Outline_Embolden(outline, (FT_Pos)lround(fit->embolden * scale * 64))) return false;
    FT_BBox box;
    FT_Outline_Get_CBox(outline, &box);
    double width = (box.xMax - box.xMin) / 64.0;
    double left, right;
    glyph_bounds(glyph, cw, &left, &right);
    double target = (right - left) * scale;
    if (width > target && width > 0) {
        double factor = target / width;
        if (factor < 0.85) factor = 0.85;
        FT_Matrix matrix = {(FT_Fixed)lround(factor * 65536), 0, 0, 65536};
        FT_Outline_Transform(outline, &matrix);
        FT_Outline_Get_CBox(outline, &box);
        *condensed = true;
    }
    FT_Pos shift = (FT_Pos)lround(left * scale * 64 + (target * 64 - (box.xMax - box.xMin)) / 2) - box.xMin;
    FT_Outline_Translate(outline, shift, 0);
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) return false;
    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap *bm = &slot->bitmap;
    if (bm->pixel_mode != FT_PIXEL_MODE_GRAY && bm->width && bm->rows) return false;
    int width_pixels = cw * scale, height_pixels = ch * scale;
    int top = (int)lround(fit->baseline * scale) - slot->bitmap_top;
    for (unsigned y = 0; y < bm->rows; y++) for (unsigned x = 0; x < bm->width; x++) {
        int dx = slot->bitmap_left + (int)x, dy = top + (int)y;
        if (dx < 0 || dx >= width_pixels || dy < 0 || dy >= height_pixels ||
            dx + 0.5 < left * scale || dx + 0.5 >= right * scale) continue;
        const uint8_t *row = bm->pitch >= 0 ? bm->buffer + y * bm->pitch :
                             bm->buffer + (bm->rows - 1 - y) * -bm->pitch;
        cell[(height_pixels - 1 - dy) * width_pixels + dx] = row[x];
    }
    return true;
}

static bool valid(size_t count, int w, int h, int cw, int ch, int cols, int rows, int scale) {
    return w > 0 && h > 0 && cw > 0 && ch > 0 && cols > 0 && rows > 0 && scale > 0 && scale <= 4 &&
           count <= (size_t)cols * rows && cols * (cw + 1) <= w + 1 && rows * (ch + 1) <= h + 1 &&
           cw <= 128 && ch <= 128;
}

static void copy_cell(uint8_t *output, const uint8_t *cell, int w, int h, int cw, int ch, int cols,
                      int scale, size_t i) {
    int x = (int)(i % cols) * (cw + 1) * scale;
    int y = (h - ((int)(i / cols) * (ch + 1) + ch)) * scale;
    for (int j = 0; j < ch * scale; j++)
        memcpy(output + (size_t)(y + j) * w * scale + x, cell + (size_t)j * cw * scale, cw * scale);
}
// Unused atlas gutters and every cell edge must be empty for the game's bilinear sampling.
static void clear_sheet_guard(uint8_t *output, int w, int h, int cw, int ch, int cols, int rows, int scale) {
    int stride = w * scale, cell_w = cw * scale, cell_h = ch * scale;
    for (int row = 0; row < rows; row++) {
        int y0 = (h - row * (ch + 1) - ch) * scale;
        if (y0 >= scale) memset(output + (size_t)(y0 - scale) * stride, 0, (size_t)scale * stride);
        for (int y = 0; y < cell_h; y++) for (int col = 0; col < cols; col++) {
            int x0 = col * (cw + 1) * scale;
            uint8_t *p = output + (size_t)(y0 + y) * stride + x0;
            if (y < 2 || y >= cell_h - 2) memset(p, 0, cell_w);
            else {
                memset(p, 0, 2);
                memset(p + cell_w - 2, 0, 2);
            }
            int gap = stride - x0 - cell_w;
            if (gap > scale) gap = scale;
            if (gap > 0) memset(p + cell_w, 0, gap);
        }
    }
}


static double source_height(const uint8_t *source, int w, int h, int cw, int ch, int cols, size_t i) {
    int x0 = (int)(i % cols) * (cw + 1);
    int y0 = h - ((int)(i / cols) * (ch + 1) + ch);
    int min_y = ch, max_y = -1;
    for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {
        if (source[(y0 + y) * w + x0 + x] <= 128) continue;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    return max_y >= min_y ? max_y - min_y + 1 : 0;
}

static double font_height(FT_Face face, uint32_t code, int scale) {
    FT_UInt index = FT_Get_Char_Index(face, code);
    if (!index || FT_Load_Glyph(face, index, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) ||
        face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return 0;
    FT_BBox box;
    FT_Outline_Get_CBox(&face->glyph->outline, &box);
    return (box.yMax - box.yMin) / (64.0 * scale);
}

// Pick the smallest upward baseline shift that maximises the shared ASCII scale.
static bool constrain_sheet_height(FT_Library library, FT_Face face, const PlayerFontGlyph *glyphs, size_t count,
                                   int ch, int scale, double max_shift, PlayerFontFit *fit) {
    if (!prepare_face(library, face, fit, scale)) return false;
    double ascent = 0, descent = 0;
    for (size_t i = 0; i < count; i++) {
        if (glyphs[i].code < 0x20 || glyphs[i].code > 0x7e) continue;
        FT_UInt index = FT_Get_Char_Index(face, glyphs[i].code);
        if (!index || FT_Load_Glyph(face, index, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) ||
            face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) continue;
        FT_BBox box;
        FT_Outline_Get_CBox(&face->glyph->outline, &box);
        if (box.yMax / 64.0 > ascent) ascent = box.yMax / 64.0;
        if (-box.yMin / 64.0 > descent) descent = -box.yMin / 64.0;
    }
    int baseline = (int)lround(fit->baseline * scale);
    int max_step = (int)floor(max_shift * scale);
    if (max_step > scale) max_step = scale;
    if (max_step < 0) max_step = 0;
    double factor = 0;
    int selected = 0;
    for (int shift = 0; shift <= max_step; shift++) {
        double top_room = baseline - shift - 3;
        double bottom_room = ch * scale - 3 - (baseline - shift);
        if (top_room <= 0 || bottom_room <= 0) continue;
        double candidate = 1;
        if (ascent > top_room) candidate = top_room / ascent;
        if (descent > bottom_room && bottom_room / descent < candidate) candidate = bottom_room / descent;
        if (candidate > factor) { factor = candidate; selected = shift; }
    }
    if (factor <= 0) return false;
    fit->baseline_shift = (double)selected / scale;
    fit->baseline -= fit->baseline_shift;
    fit->scale_factor = 1;
    if (factor < 1) {
        double original = fit->scale;
        fit->scale = floor(fit->scale * scale * 64 * factor - 1) / (scale * 64);
        if (fit->scale <= 0) return false;
        fit->scale_factor = fit->scale / original;
    }
    return true;
}


// The reconstruction's fitted transfer curve preserves the game's shader thresholds, not raw AA ink.
static bool font_tone(FT_Library library, FT_Face face, const PlayerFontGlyph *glyphs, size_t count,
                      const uint8_t *source, int w, int h, int cw, int ch, int cols, int scale,
                      const PlayerFontFit *fit, uint8_t *cell, uint8_t lut[256]) {
    uint64_t original[256] = {0}, rendered[256] = {0};
    if (!prepare_face(library, face, fit, scale)) return false;
    size_t found = 0;
    for (size_t i = 0; i < count; i++) {
        if (!glyphs[i].code) continue;
        bool condensed;
        if (!draw_v1(face, &glyphs[i], cw, ch, scale, fit, cell, &condensed, NULL)) continue;
        found++;
        int x0 = (int)(i % cols) * (cw + 1);
        int y0 = h - ((int)(i / cols) * (ch + 1) + ch);
        for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++)
            original[source[(y0 + y) * w + x0 + x]]++;
        for (int k = 0; k < cw * ch * scale * scale; k++) rendered[cell[k]]++;
    }
    if (!found) return false;
    text_fit_tone(original, rendered, scale, lut);
    return true;
}


bool player_font_render(const char *path, const PlayerFontGlyph *glyphs, size_t count, int w, int h, int cw, int ch,
                        int cols, int rows, int scale, const PlayerFontFit *fit, uint8_t *output) {
    if (!path || !glyphs || !fit || !output || !valid(count, w, h, cw, ch, cols, rows, scale)) return false;
    FT_Library library;
    FT_Face face;
    if (FT_Init_FreeType(&library)) return false;
    if (FT_New_Face(library, path, 0, &face)) { FT_Done_FreeType(library); return false; }
    uint8_t *cell = malloc((size_t)cw * ch * scale * scale);
    bool ok = cell && prepare_face(library, face, fit, scale);
    if (ok) for (size_t i = 0; i < count; i++) {
        bool condensed;
        if (glyphs[i].code && draw(face, &glyphs[i], cw, ch, scale, fit, cell, &condensed, NULL))
            copy_cell(output, cell, w, h, cw, ch, cols, scale, i);
    }
    free(cell);
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return ok;
}

bool player_font_fit(const char *path, const PlayerFontGlyph *glyphs, size_t count, const uint8_t *source,
                     uint8_t *output, int w, int h, int cw, int ch, int cols, int rows, int scale,
                     double baseline, const PlayerFontPreset *preset, PlayerFontFit *fit) {
    if (!path || !glyphs || !source || !output || !fit || baseline < 0 || baseline > ch ||
        !valid(count, w, h, cw, ch, cols, rows, scale)) return false;
    FT_Library library;
    FT_Face face;
    if (FT_Init_FreeType(&library)) return false;
    if (FT_New_Face(library, path, 0, &face)) { FT_Done_FreeType(library); return false; }
    uint8_t *cell = malloc((size_t)cw * ch * scale * scale);
    if (!cell) { FT_Done_Face(face); FT_Done_FreeType(library); return false; }
    double min_weight = 0, max_weight = 0, initial_weight = 0;
    double min_width = 0, max_width = 0, initial_width = 0;
    bool variable = axis_info(library, face, FT_MAKE_TAG('w', 'g', 'h', 't'),
                              &min_weight, &max_weight, &initial_weight);
    bool variable_width = axis_info(library, face, FT_MAKE_TAG('w', 'd', 't', 'h'),
                                    &min_width, &max_width, &initial_width);
    if (preset && ((!variable && preset->weight) || (!variable_width && preset->width_axis) ||
                   (preset->weight && (preset->weight < min_weight || preset->weight > max_weight)) ||
                   (preset->width_axis && (preset->width_axis < min_width || preset->width_axis > max_width))))
        goto fail;
    PlayerFontFit best = {.scale = ch, .baseline = baseline, .weight = initial_weight, .width_axis = initial_width};
    if (!prepare_face(library, face, &best, scale)) goto fail;
    double caps[26], lower[26], fcaps[26], flower[26];
    size_t ncaps = 0, nlower = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t code = glyphs[i].code;
        if (!((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z'))) continue;
        double target = source_height(source, w, h, cw, ch, cols, i);
        double actual = font_height(face, code, scale);
        if (!target || !actual) continue;
        if (code <= 'Z' && ncaps < 26) { caps[ncaps] = target; fcaps[ncaps++] = actual; }
        if (code >= 'a' && nlower < 26) { lower[nlower] = target; flower[nlower++] = actual; }
    }
    best.cap_height = median(caps, ncaps);
    best.x_height = median(lower, nlower);
    double fcap = median(fcaps, ncaps), flower_height = median(flower, nlower);
    double denominator = fcap * fcap + flower_height * flower_height;
    if (!denominator) goto fail;
    best.scale *= (best.cap_height * fcap + best.x_height * flower_height) / denominator;
    if (best.scale < 2 || best.scale > ch * 2) goto fail;
    if (preset && preset->weight) best.weight = preset->weight;
    if (preset && preset->width_axis) best.width_axis = preset->width_axis;
    uint8_t lut[256];
    // Use the first build's unguarded raster only to recover its sheet-wide tone curve.
    if (!font_tone(library, face, glyphs, count, source, w, h, cw, ch, cols, scale, &best, cell, lut) ||
        !constrain_sheet_height(library, face, glyphs, count, ch, scale,
                                preset ? preset->baseline_shift_limit : 0, &best) ||
        !prepare_face(library, face, &best, scale)) goto fail;
    best.min_glyph_factor = 1;
    for (size_t i = 0; i < count; i++) {
        if (!glyphs[i].code) continue;
        bool condensed;
        double glyph_factor;
        if (draw(face, &glyphs[i], cw, ch, scale, &best, cell, &condensed, &glyph_factor)) {
            for (int k = 0; k < cw * ch * scale * scale; k++) cell[k] = lut[cell[k]];
            copy_cell(output, cell, w, h, cw, ch, cols, scale, i);
            best.fitted++;
            if (condensed) best.condensed++;
            if (glyph_factor < 1) {
                best.scaled++;
                if (glyph_factor < best.min_glyph_factor) best.min_glyph_factor = glyph_factor;
            }
        } else best.fallback++;
    }
    clear_sheet_guard(output, w, h, cw, ch, cols, rows, scale);
    *fit = best;
    free(cell);
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return true;
fail:
    free(cell);
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return false;
}
#endif
