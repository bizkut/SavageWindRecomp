// Host check of the desktop guest VI HLE used by the PS5 M2 build.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdlib.h>
#include "../../runtime/hle/vi.c"

void *hle_alloc(uint64_t size) { void *p = calloc(1, size); assert(p); return p; }
int main(void) {
    Cpu cpu = {0};
    uint64_t display = 0, layer = 0, window = 0, window_again = 0;
    cpu.x[0] = (uintptr_t)&display;
    vi_open_default_display(&cpu);
    assert(cpu.x[0] == 0 && display);
    cpu.x[0] = (uintptr_t)&layer;
    cpu.x[1] = display;
    vi_create_layer(&cpu);
    assert(cpu.x[0] == 0 && layer && layer != display);
    cpu.x[0] = (uintptr_t)&window;
    cpu.x[1] = layer;
    vi_get_native_window(&cpu);
    assert(cpu.x[0] == 0 && window);
    cpu.x[0] = (uintptr_t)&window_again;
    vi_get_native_window(&cpu);
    assert(window_again == window);
    free((void *)(uintptr_t)display);
    free((void *)(uintptr_t)layer);
    free((void *)(uintptr_t)window);
    return 0;
}
