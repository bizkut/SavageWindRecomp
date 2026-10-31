#include <assert.h>
#include <string.h>
#include "../runtime/hle/framepace.c"
Settings g_settings;
GameSites g_site;

static uint8_t objects[64][0x200];
static size_t object_count;
static uint64_t object(void) {
    assert(object_count < 64);
    uint8_t *p = objects[object_count];
    memcpy(p + 0xb8, "test", 5);
    prompt_panes[object_count].object = (uint64_t)(uintptr_t)p;
    memcpy(prompt_panes[object_count].name, "test", 5);
    object_count++;
    return (uint64_t)(uintptr_t)p;
}
static void value(uint64_t p, unsigned off, float v) { memcpy((void *)(uintptr_t)(p + off), &v, 4); }
int main(void) {
    uint64_t slots[4] = {0x111, 0x222, 0x222, 0x111};
    uint8_t materials[4][0x50] = {{0}};
    const int initial[] = {1, 0, 3, 2};
    const int target[] = {0, 1, 2, 3};
    for (int i = 0; i < 4; i++) {
        PromptLiveGroup *g = &prompt_live[i];
        int pos = initial[i];
        int left = pos == 2;
        g->root = object();
        g->loop = object();
        g->icon0 = object();
        g->group = object();
        g->capture = object();
        g->base = object();
        g->guide0 = object();
        g->guide1 = object();
        g->icon1 = object();
        g->bar = object();
        g->capture_pic = object();
        for (int j = 0; j < 4; j++) if (j != i) g->label[j] = object();
        g->slot = (uint64_t)(uintptr_t)&slots[i];
        g->material = (uint64_t)(uintptr_t)materials[i];
        uint64_t slot_base = g->slot - 8;
        memcpy(materials[i] + 0x18, &slot_base, sizeof slot_base);
        memcpy((void *)(uintptr_t)(g->base + 0xe0), &g->material, sizeof g->material);
        memcpy((void *)(uintptr_t)(g->icon0 + 0x18), &g->group, 8);
        memcpy((void *)(uintptr_t)(g->group + 0x18), &g->loop, 8);
        memcpy((void *)(uintptr_t)(g->loop + 0x18), &g->root, 8);
        memcpy((void *)(uintptr_t)(g->capture + 0x18), &g->root, 8);
        memcpy((void *)(uintptr_t)(g->base + 0x18), &g->loop, 8);
        g->original_texture = g->expected_texture = slots[i];
        g->position = g->initial_position = pos;
        value(g->group, 0x30, prompt_button_xy[pos][0]);
        value(g->group, 0x34, prompt_button_xy[pos][1]);
        value(g->capture, 0x30, prompt_button_xy[pos][0] + (left ? -60 : 60));
        value(g->capture, 0x34, prompt_button_xy[pos][1] + (i == 0 ? 4 : 0));
        value(g->guide0, 0x30, left ? 37 : -39);
        value(g->guide1, 0x30, left ? -23 : 21);
        value(g->icon1, 0x30, left ? 60 : -60);
        value(g->bar, 0x30, left ? 33 : -33);
        value(g->bar, 0x48, left ? -1 : 1);
        value(g->capture_pic, 0x30, left ? -60 : 60);
        value(g->base, 0x44, pos == 2 || pos == 3 ? 180 : 0);
        value(g->base, 0x48, pos == 3 ? -1 : 1);
        for (int j = 0; j < 4; j++) if (j != i) {
            value(g->label[j], 0x30, prompt_text_xy[initial[j]][0]);
            value(g->label[j], 0x34, prompt_text_xy[initial[j]][1]);
        }
    }
    prompt_live_ready = true;
    assert(prompt_live_apply_locked(target));
    prompt_live_modified = true;
    assert(slots[0] == 0x222 && slots[1] == 0x111 && slots[2] == 0x111 && slots[3] == 0x222);
    assert(prompt_float(prompt_live[0].group, 0x30) == 0);
    assert(prompt_float(prompt_live[0].group, 0x34) == -26);
    value(prompt_live[0].group, 0x30, 99);
    assert(!prompt_live_apply_locked(initial));
    assert(slots[0] == 0x222);
    value(prompt_live[0].group, 0x30, 0);
    g_site.main_data = 0x7001b7a000ull;
    prompt_debug_entry(0x70000287c4ull, prompt_live[0].base, 0, 0, 0);
    assert(!prompt_live_ready);
    assert(slots[0] == 0x111 && slots[1] == 0x222 && slots[2] == 0x222 && slots[3] == 0x111);
    return 0;
}
