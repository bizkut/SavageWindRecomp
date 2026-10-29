#include <assert.h>
#include "../runtime/ps5/dynamic_resolution.h"
#include <math.h>

static void close_to(float a, float b) { assert(fabsf(a - b) < 0.0001f); }

int main(void) {
    DynamicResolution d = {0};
    assert(dynamic_resolution_update(&d, true, false, 16, 0, 1.0/60) == 0);
    assert(dynamic_resolution_update(&d, true, false, 16, 0, 1.0/60) == 0);
    assert(dynamic_resolution_update(&d, true, false, 16, 0, 1.0/60) == 1);
    for (int i = 0; i < 3; i++) dynamic_resolution_update(&d, true, false, 16, 0, 1.0/60);
    assert(d.step == 2);
    for (int i = 0; i < 119; i++) dynamic_resolution_update(&d, true, false, 5, 0, 1.0/60);
    assert(d.step == 2);
    for (int i = 0; i < 2; i++) dynamic_resolution_update(&d, true, false, 5, 0, 1.0/60);
    assert(d.step == 1);
    assert(dynamic_resolution_update(&d, true, true, 20, 0, 1.0/60) == 0);
    for (int i = 0; i < 3; i++) dynamic_resolution_update(&d, true, false, 13, 3, 1.0/60);
    assert(d.step == 1);
    assert(dynamic_resolution_update(&d, false, false, 20, 0, 1.0/60) == 0);
    assert(dynamic_resolution_allowed(true, false, true, true));
    assert(!dynamic_resolution_allowed(true, false, true, false));
    assert(!dynamic_resolution_allowed(true, true, true, true));
    assert(!dynamic_resolution_allowed(false, false, true, true));
    assert(dynamic_resolution_allowed(true, false, false, false));
    assert(!dynamic_resolution_remap(false, false, 1.0f));
    assert(dynamic_resolution_remap(true, false, 1.0f));
    assert(!dynamic_resolution_remap(true, true, 1.0f));
    assert(dynamic_resolution_remap(true, true, 0.835f));
    assert(!dynamic_resolution_remap(false, true, 0.835f));
    close_to(dynamic_resolution_level_fraction(1440, 0, true), 1.0f);
    close_to(dynamic_resolution_level_fraction(1440, 1, true), 0.835f);
    close_to(dynamic_resolution_level_fraction(1440, 2, true), 0.67f);
    close_to(dynamic_resolution_level_fraction(1080, 2, true), 0.67f);
    close_to(dynamic_resolution_level_fraction(1000, 2, true), 0.72f);
    close_to(dynamic_resolution_level_fraction(900, 1, true), 0.9f);
    close_to(dynamic_resolution_level_fraction(700, 2, true), 1.0f);
    close_to(dynamic_resolution_level_fraction(0, 2, true), 1.0f);
    close_to(dynamic_resolution_level_fraction(1440, 1, false), 900.0f / 1440.0f);
    close_to(dynamic_resolution_level_fraction(1440, 2, false), 0.5f);
    return 0;
}
