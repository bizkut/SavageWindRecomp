#include <assert.h>
#include "../runtime/ps5/dynamic_resolution.h"

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
    return 0;
}
