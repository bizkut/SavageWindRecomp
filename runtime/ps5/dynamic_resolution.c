#include "dynamic_resolution.h"

int dynamic_resolution_update(DynamicResolution *dr, bool enabled, bool temporal,
                              double gpu_ms, double fg_ms, double frame_seconds) {
    if (!enabled || temporal) {
        dr->step = dr->over_frames = 0;
        dr->under_seconds = 0;
        return 0;
    }
    if (gpu_ms <= 0 || frame_seconds <= 0) return dr->step;
    double budget = 16.7 - (fg_ms > 0 ? fg_ms : 0) - 1.0;
    if (budget < 1.0) budget = 1.0;
    if (gpu_ms > budget) {
        dr->under_seconds = 0;
        if (++dr->over_frames >= 3) {
            if (dr->step < 2) dr->step++;
            dr->over_frames = 0;
        }
    } else {
        dr->over_frames = 0;
        if (gpu_ms < budget * 0.8) {
            dr->under_seconds += frame_seconds;
            if (dr->under_seconds >= 2.0) {
                if (dr->step > 0) dr->step--;
                dr->under_seconds = 0;
            }
        } else dr->under_seconds = 0;
    }

    return dr->step;
}
#ifdef __PROSPERO__
#include "../platform.h"
#include "../settings.h"
#include <math.h>
#include <stdint.h>

static DynamicResolution state;
static uint64_t last_sample;
static double fg_ms;
float g_dynamic_resolution_fraction = 1.0f;
bool g_dynamic_resolution_mode;
static unsigned internal_w, internal_h;

void dynamic_resolution_fg_gpu(uint64_t ns) { fg_ms = ns / 1e6; }

void dynamic_resolution_gpu(uint64_t ns) {
    if (!g_settings.dynamic_resolution || g_settings.antialias == AA_TAA || g_settings.upscaler == UPSCALE_FSR3) {
        if (last_sample) { state = (DynamicResolution){0}; last_sample = 0; }
        return;
    }
    uint64_t now = plat_now_ns();
    double dt = last_sample && now > last_sample ? (now - last_sample) / 1e9 : 1.0 / 60.0;
    last_sample = now;
    if (dt > 0.25) dt = 0.25;
    double game_ms = ns / 1e6;
    double generation_ms = g_settings.frame_gen ? fg_ms : 0;
    if (game_ms > generation_ms) game_ms -= generation_ms;
    dynamic_resolution_update(&state, g_settings.dynamic_resolution,
                              g_settings.antialias == AA_TAA || g_settings.upscaler == UPSCALE_FSR3,
                              game_ms, generation_ms, dt);
}

void dynamic_resolution_begin_frame(float sx, float sy) {
    g_dynamic_resolution_mode = g_settings.dynamic_resolution && g_settings.antialias != AA_TAA && g_settings.upscaler != UPSCALE_FSR3;
    int step = g_dynamic_resolution_mode ? state.step : 0;
    float height = 900.0f * sy;
    float target = step == 1 ? 900.0f : step == 2 ? 720.0f : height;
    g_dynamic_resolution_fraction = height > 0 && target < height ? target / height : 1.0f;
    __atomic_store_n(&internal_w, (unsigned)lroundf(1600.0f * sx * g_dynamic_resolution_fraction), __ATOMIC_RELAXED);
    __atomic_store_n(&internal_h, (unsigned)lroundf(height * g_dynamic_resolution_fraction), __ATOMIC_RELAXED);
}

void dynamic_resolution_size(unsigned *w, unsigned *h) {
    *w = __atomic_load_n(&internal_w, __ATOMIC_RELAXED);
    *h = __atomic_load_n(&internal_h, __ATOMIC_RELAXED);
}
#endif
