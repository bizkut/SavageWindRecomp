// Exercise the same drawing and binding path used by the action texture and the dim font glyphs.
#include <assert.h>
#include <string.h>
#include "../runtime/nvn/glyphs.c"

Settings g_settings;
int16_t g_bind_pad[BIND_PAD_COUNT];
int16_t g_bind_key[BIND_KEY_COUNT];
int input_last_device(void) { return DEVICE_PAD; }
int input_pad_family(void) { return 2; }
const char *SDL_GetScancodeName(SDL_Scancode code) { (void)code; return "Test"; }
bool rt_resource(const char *name, const char *dev, char *out, size_t n) {
    (void)name;
    return snprintf(out, n, "%s", dev) > 0;
}
const char *bind_pad_label(int host, int family) {
    (void)host;
    (void)family;
    return "?";
}

int main(void) {
    const int positions[2][4] = {
        {SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_WEST},
        {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH},
    };
    const int keys[4] = {SDL_SCANCODE_E, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_Q};
    for (int i = 0; i < 4; i++) g_bind_key[BIND_A + i] = keys[i];
    Box bounds = {25, 25, 40, 40};
    for (int layout = 0; layout < 2; layout++) {
        for (int i = 0; i < 4; i++) g_bind_pad[BIND_A + i] = positions[layout][i];
        for (int kind = ICONS_XBOX; kind <= ICONS_PLAYSTATION; kind++) {
            for (int i = 0; i < 4; i++) {
                Canvas actual = {0}, expected = {0};
                int copy = 0;
                assert(draw_glyph(&actual, bounds, 0xE040 + i, kind, &copy));
                pad_button(&expected, bounds, positions[layout][i], kind == ICONS_PLAYSTATION);
                assert(!copy && !memcmp(&actual, &expected, sizeof actual));
            }
        }
        for (int i = 0; i < 4; i++) {
            Canvas canvas = {0};
            int copy = 0;
            assert(!draw_glyph(&canvas, bounds, 0xE040 + i, ICONS_NINTENDO, &copy));
            int expected = nintendo_code(positions[layout][i]);
            assert(copy == (expected == 0xE040 + i ? 0 : expected));
        }
        for (int i = 0; i < 4; i++) {
            Canvas actual = {0}, expected = {0};
            int copy = 0;
            assert(draw_glyph(&actual, bounds, 0xE040 + i, ICONS_KEYBOARD, &copy));
            key_icon(&expected, bounds, keys[i]);
            assert(!copy && !memcmp(&actual, &expected, sizeof actual));
        }
    }
    Canvas touchpad = {0}, expected = {0}, create = {0};
    int copy = 0;
    g_bind_pad[BIND_MINUS] = SDL_GAMEPAD_BUTTON_TOUCHPAD;
    assert(meaning(0xE04C).kind == G_BUTTON && meaning(0xE04C).arg == BIND_MINUS);
    assert(draw_glyph(&touchpad, bounds, 0xE04C, ICONS_PLAYSTATION, &copy) && !copy);
    fill(&expected, ring(box(bounds.cx, bounds.cy, minf(bounds.w * 0.46f, bounds.s * 0.60f),
                             bounds.s * 0.28f, bounds.s * 0.10f), bounds.s * 0.06f));
    assert(!memcmp(&touchpad, &expected, sizeof touchpad));
    uint32_t generation = glyphs_generation();
    g_bind_pad[BIND_MINUS] = SDL_GAMEPAD_BUTTON_BACK;
    assert(draw_glyph(&create, bounds, 0xE04C, ICONS_PLAYSTATION, &copy));
    assert(memcmp(&touchpad, &create, sizeof touchpad));
    assert(glyphs_generation() != generation);
    return 0;
}
