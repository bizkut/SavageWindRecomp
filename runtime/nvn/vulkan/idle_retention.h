#pragma once
#include <stdint.h>

#define IDLE_KEEP_BASE_BUDGET (192ull << 20)
#define IDLE_KEEP_BASE_IMAGE (16ull << 20)
#define IDLE_KEEP_CEILING (768ull << 20)

// Images grow with area. Bound the aggregate budget even at very large output scales.
static inline double idle_keep_area(float sx, float sy) {
    double area = (double)sx * sy;
    return area > 1.0 ? (area < 4.0 ? area : 4.0) : 1.0;
}
static inline uint64_t idle_keep_budget(float sx, float sy) {
    return (uint64_t)(IDLE_KEEP_BASE_BUDGET * idle_keep_area(sx, sy));
}
static inline uint64_t idle_keep_image_limit(float sx, float sy) {
    return (uint64_t)(IDLE_KEEP_BASE_IMAGE * idle_keep_area(sx, sy));
}
static inline int idle_keep_over_budget(uint64_t used, float sx, float sy) {
    return used > idle_keep_budget(sx, sy);
}
