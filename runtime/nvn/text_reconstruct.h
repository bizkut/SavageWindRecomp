// Monotone cubic reconstruction of font coverage, independently within each atlas cell.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

// A shape-preserving Hermite slope prevents ringing around the game's outline values.
static float text_slope(float a, float b) {
    if (a * b <= 0) return 0;
    return 2 * a * b / (a + b);
}

static float text_cubic(float a, float b, float c, float d, float t) {
    float m0 = text_slope(b - a, c - b);
    float m1 = text_slope(c - b, d - c);
    float t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * b + (t3 - 2 * t2 + t) * m0 +
           (-2 * t3 + 3 * t2) * c + (t3 - t2) * m1;
}

static int text_clamp(int x, int max) {
    return x < 0 ? 0 : x > max ? max : x;
}

static float text_threshold(float x, float lo, float hi) {
    x = (x - lo) / (hi - lo);
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    return x * x * (3 - 2 * x);
}

static float text_tone(int v, float slope, float offset) {
    if (v == 0) return 0;
    if (v == 255) return 1;
    float x = (v / 255.0f - 0.5f) * slope + 0.5f + offset;
    return x < 0 ? 0 : x > 1 ? 1 : x;
}

static void text_fit_tone(const uint64_t original[256], const uint64_t reconstructed[256], int scale,
                          uint8_t lut[256]) {
    // One tone curve for the whole sheet, not an independent weight for each glyph.
    double target[3] = {0, 0, 0};
    for (int k = 0; k < 256; k++) {
        target[0] += original[k] * text_threshold(k / 255.0f, 0.4f, 0.6f) * scale * scale;
        target[1] += original[k] * text_threshold(k / 255.0f, 0.2f, 0.5f) * scale * scale;
        target[2] += original[k] * (k / 255.0) * scale * scale;
    }
    float best_slope = 1, best_offset = 0;
    double best_error = 1e30;
    uint64_t high = 0;
    for (int k = 220; k < 256; k++) high += reconstructed[k];
    for (int si = 0; si <= 32; si++) {
        float slope = 0.5f + si * 0.0625f, lo = -0.25f, hi = 0.25f;
        for (int it = 0; it < 16; it++) {
            float mid = (lo + hi) * 0.5f;
            double sum = 0;
            for (int k = 0; k < 256; k++)
                sum += reconstructed[k] * text_threshold(text_tone(k, slope, mid), 0.4f, 0.6f);
            if (sum < target[0]) lo = mid;
            else hi = mid;
        }
        for (int oi = -16; oi <= 16; oi++) {
            float offset = (lo + hi) * 0.5f + oi * 0.0025f;
            double actual[3] = {0, 0, 0};
            uint64_t output_high = 0;
            for (int k = 0; k < 256; k++) {
                float v = text_tone(k, slope, offset);
                actual[0] += reconstructed[k] * text_threshold(v, 0.4f, 0.6f);
                actual[1] += reconstructed[k] * text_threshold(v, 0.2f, 0.5f);
                actual[2] += reconstructed[k] * v;
                if (v * 255 >= 220) output_high += reconstructed[k];
            }
            double error = 0;
            for (int t = 0; t < 3; t++) {
                double e = fabs(actual[t] - target[t]) / (target[t] > 1 ? target[t] : 1);
                if (e > error) error = e;
            }
            if (high && output_high * 5 < high * 4) {
                double e = (double)(high * 4 - output_high * 5) / (high * 5);
                if (e > error) error = e;
            }
            if (error < best_error) {
                best_error = error;
                best_slope = slope;
                best_offset = offset;
            }
        }
    }
    for (int k = 0; k < 256; k++) lut[k] = (uint8_t)(255 * text_tone(k, best_slope, best_offset) + 0.5f);
    int source_peak = 255, base_peak = 255;
    while (source_peak && !original[source_peak]) source_peak--;
    while (base_peak && !reconstructed[base_peak]) base_peak--;
    int output_peak = lut[base_peak];
    if (source_peak > 153 && output_peak > 153 && output_peak < source_peak) {
        for (int k = 0; k < 256; k++) if (lut[k] > 153) {
            int value = 153 + (lut[k] - 153) * (source_peak - 153) / (output_peak - 153);
            lut[k] = (uint8_t)(value > 255 ? 255 : value);
        }
    }
    // The shader thresholds do not constrain bright interior values. Adjust only
    // those values for raw ink while retaining fully opaque cores.
    double raw = 0, down = 0, up = 0;
    for (int k = 0; k < 256; k++) {
        int v = lut[k];
        raw += reconstructed[k] * v;
        if (v > 153 && v < 255) {
            down += reconstructed[k] * (v - (v >= 220 ? 220 : 153));
            up += reconstructed[k] * (255 - v);
        }
    }
    double delta = target[2] * 255 - raw;
    double capacity = delta < 0 ? down : up;
    if (capacity > 0) {
        double fraction = fabs(delta) / capacity;
        if (fraction > 1) fraction = 1;
        for (int k = 0; k < 256; k++) {
            int v = lut[k];
            if (v > 153 && v < 255) {
                double change = delta < 0 ? v - (v >= 220 ? 220 : 153) : 255 - v;
                lut[k] = (uint8_t)(v + (delta < 0 ? -fraction : fraction) * change + 0.5);
            }
        }
    }
}

static void text_reconstruct_sheet(const uint8_t *src, uint8_t *dst, int w, int h, int cw, int ch,
                                   int cols, int rows, int scale) {
    int ow = w * scale;
    uint64_t original[256] = {0}, reconstructed[256] = {0};
    for (int row = 0; row < rows; row++) for (int col = 0; col < cols; col++) {
        int x0 = col * (cw + 1), y0 = h - row * (ch + 1) - ch;
        unsigned sum = 0;
        for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {
            int v = src[(y0 + y) * w + x0 + x];
            original[v]++;
            sum += v;
        }
        if (!sum) {
            reconstructed[0] += (uint64_t)cw * ch * scale * scale;
            for (int y = 0; y < ch * scale; y++)
                memset(dst + (size_t)(y0 * scale + y) * ow + x0 * scale, 0, cw * scale);
            continue;
        }
        for (int y = 0; y < ch * scale; y++) {
            float sy = (y + 0.5f) / scale - 0.5f;
            int iy = (int)floorf(sy);
            float fy = sy - iy;
            for (int x = 0; x < cw * scale; x++) {
                float sx = (x + 0.5f) / scale - 0.5f;
                int ix = (int)floorf(sx);
                float fx = sx - ix, v[4];
                for (int j = 0; j < 4; j++) {
                    int py = y0 + text_clamp(iy + j - 1, ch - 1);
                    float a = src[py * w + x0 + text_clamp(ix - 1, cw - 1)];
                    float b = src[py * w + x0 + text_clamp(ix, cw - 1)];
                    float c = src[py * w + x0 + text_clamp(ix + 1, cw - 1)];
                    float d = src[py * w + x0 + text_clamp(ix + 2, cw - 1)];
                    v[j] = text_cubic(a, b, c, d, fx);
                }
                float value = text_cubic(v[0], v[1], v[2], v[3], fy);
                if (value < 0) value = 0;
                if (value > 255) value = 255;
                uint8_t output = (uint8_t)(value + 0.5f);
                dst[(y0 * scale + y) * ow + x0 * scale + x] = output;
                reconstructed[output]++;
            }
        }
    }
    uint8_t lut[256];
    text_fit_tone(original, reconstructed, scale, lut);
    for (int row = 0; row < rows; row++) for (int col = 0; col < cols; col++) {
        int x0 = col * (cw + 1) * scale, y0 = (h - row * (ch + 1) - ch) * scale;
        for (int y = 0; y < ch * scale; y++) for (int x = 0; x < cw * scale; x++) {
            size_t p = (size_t)(y0 + y) * ow + x0 + x;
            dst[p] = lut[dst[p]];
        }
    }
}
