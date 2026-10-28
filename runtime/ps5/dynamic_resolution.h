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

#ifdef __PROSPERO__
#include <stdint.h>
void dynamic_resolution_gpu(uint64_t ns);
void dynamic_resolution_fg_gpu(uint64_t ns);
void dynamic_resolution_begin_frame(float scene_sx, float scene_sy);
float dynamic_resolution_fraction(void);
void dynamic_resolution_size(unsigned *w, unsigned *h);
#endif
