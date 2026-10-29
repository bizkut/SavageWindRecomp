// PS5 dynamic-resolution controller, independent of Vulkan for host testing.
#pragma once
#include <stdbool.h>

typedef struct {
    int step;
    int over_frames;
    double under_seconds;
} DynamicResolution;

// A sample is one completed game frame. fg_ms is the measured generation cost.
// Returns 0 for the configured size, 1 for 900p, and 2 for 720p.
int dynamic_resolution_update(DynamicResolution *dr, bool enabled, bool temporal,
                              double gpu_ms, double fg_ms, double frame_seconds);

// Eligibility is shared with startup precompilation, before the first render frame.
static inline bool dynamic_resolution_allowed(bool enabled, bool taa, bool fsr3, bool allow_fsr3) {
    return enabled && !taa && (!fsr3 || allow_fsr3);
}
// Lean mode uses ordinary shaders at full size and remapping shaders at reduced size.
static inline bool dynamic_resolution_remap(bool enabled, bool lean, float fraction) {
    return enabled && (!lean || fraction < 1.0f);
}
// FSR 3 levels: full size, midpoint, then max(720p, 67% of full), never above full.
float dynamic_resolution_level_fraction(float height, int step, bool fsr3);


#ifdef __PROSPERO__
#include <stdint.h>
void dynamic_resolution_gpu(uint64_t ns);
void dynamic_resolution_fg_gpu(uint64_t ns);
void dynamic_resolution_begin_frame(float scene_sx, float scene_sy);
extern bool g_dynamic_resolution_mode;
extern float g_dynamic_resolution_fraction;
extern bool g_dynamic_resolution_fsr3_mode;
static inline bool dynamic_resolution_mode(void) { return g_dynamic_resolution_mode; }
static inline float dynamic_resolution_fraction(void) { return g_dynamic_resolution_fraction; }
static inline bool dynamic_resolution_fsr3_mode(void) { return g_dynamic_resolution_fsr3_mode; }
void dynamic_resolution_size(unsigned *w, unsigned *h);
#endif
