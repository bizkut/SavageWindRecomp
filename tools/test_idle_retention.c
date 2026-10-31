#include <assert.h>
#include "../runtime/nvn/vulkan/idle_retention.h"

int main(void) {
    const uint64_t mib = 1ull << 20;
    assert(idle_keep_budget(1.0f, 1.0f) == 192 * mib);
    assert(idle_keep_budget(0.5f, 0.5f) == 192 * mib);
    assert(idle_keep_budget(4.0f, 4.0f) == IDLE_KEEP_CEILING);
    assert(idle_keep_image_limit(1.6f, 1.6f) > 15 * mib);
    // Six parked world images and a Slate-like set of additional images; neither the world nor Slate
    // needs eviction until the aggregate area-scaled budget is exhausted. Unparking frees budget.
    uint64_t used = 0, world[6];
    for (int i = 0; i < 6; i++) { world[i] = 15 * mib; used += world[i]; }
    for (int i = 0; i < 20; i++) used += 15 * mib;
    assert(idle_keep_over_budget(used, 1.0f, 1.0f));
    assert(!idle_keep_over_budget(used, 1.6f, 1.6f));
    for (int i = 0; i < 6; i++) { assert(world[i] == 15 * mib); used -= world[i]; }
    assert(used == 300 * mib);
}
