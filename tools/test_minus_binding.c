#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "../runtime/hle.h"
#include "../runtime/input_bind.h"
#ifdef TEST_PS5_DEFAULT
#define __PROSPERO__ 1
#endif
#include "../runtime/hle/bindings.c"

SDL_Scancode SDL_GetScancodeFromName(const char *name) {
    (void)name;
    return SDL_SCANCODE_UNKNOWN;
}
const char *SDL_GetScancodeName(SDL_Scancode scancode) {
    (void)scancode;
    return "";
}
Uint64 SDL_GetTicks(void) { return 100; }

int main(void) {
#ifdef TEST_PS5_DEFAULT
    const int expected = SDL_GAMEPAD_BUTTON_TOUCHPAD;
#else
    const int expected = SDL_GAMEPAD_BUTTON_BACK;
#endif
    assert(g_bind_pad[BIND_MINUS] == expected);
#ifdef TEST_PS5_DEFAULT
    for (int i = 0; i < BIND_PAD_COUNT; i++) assert(g_bind_pad[i] != SDL_GAMEPAD_BUTTON_BACK);
#endif
    assert(bind_settings_load("bind_pad_a", "east"));
    assert(g_bind_pad[BIND_MINUS] == expected);
    assert(bind_settings_load("bind_pad_minus", "back"));
#ifdef TEST_PS5_DEFAULT
    assert(g_bind_pad[BIND_MINUS] == SDL_GAMEPAD_BUTTON_TOUCHPAD);
    assert(bind_settings_load("bind_pad_a", "north"));
    assert(bind_settings_load("bind_pad_a", "back"));
    assert(g_bind_pad[BIND_A] == SDL_GAMEPAD_BUTTON_EAST);
#else
    assert(g_bind_pad[BIND_MINUS] == SDL_GAMEPAD_BUTTON_BACK);
#endif
    FILE *f = tmpfile();
    assert(f);
    bind_settings_save(f);
    rewind(f);
    char lines[1024] = {0};
    assert(fread(lines, 1, sizeof lines - 1, f) < sizeof lines);
    fclose(f);
    assert(!strstr(lines, "bind_pad_minus = "));
    assert(!strstr(lines, " = back\n"));
    bind_reset(BIND_DEV_PAD);
    assert(g_bind_pad[BIND_MINUS] == expected);
    SDL_Event event = {0};
    event.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    event.gbutton.button = SDL_GAMEPAD_BUTTON_BACK;
    bind_capture_begin(BIND_DEV_PAD, BIND_A);
    assert(bind_capture_event(&event));
#ifdef TEST_PS5_DEFAULT
    assert(g_bind_pad[BIND_A] == SDL_GAMEPAD_BUTTON_EAST);
    assert(bind_capture_active(NULL, NULL, NULL));
    event.gbutton.button = SDL_GAMEPAD_BUTTON_TOUCHPAD;
    assert(bind_capture_event(&event));
    assert(!bind_capture_active(NULL, NULL, NULL));
    assert(g_bind_pad[BIND_A] == SDL_GAMEPAD_BUTTON_TOUCHPAD);
#else
    assert(!bind_capture_active(NULL, NULL, NULL));
    assert(g_bind_pad[BIND_A] == SDL_GAMEPAD_BUTTON_BACK);
#endif
    return 0;
}
