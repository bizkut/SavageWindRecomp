// Mouse wheel zoom (runtime/hle/wheel_zoom.c): while the Sheikah Slate's scope (or its camera) or the map is open,
// each notch of the wheel is one pulse of the Switch input that zooms there; anywhere else the wheel does nothing. No
// binding is involved: the pulse goes to the game beside the player's own input (runtime/hle/input.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { WZ_NONE, WZ_SCOPE, WZ_MAP };

// What is open, from the game's UI screens: screen_mgr is the ScreenMgr pointer's address (game site screen_mgr),
// state_field the offset of a screen's open state byte (game site screen_state; 0: the game code has none)
int wheel_zoom_context(uint64_t screen_mgr, uint64_t state_field);

// The wheel turned y notches (> 0: away from the player, zoom in). Main thread; ignored while nothing zooms.
void wheel_zoom_wheel(float y);

// This controller read's pulse (game thread): Switch buttons to add (nn::hid bits); when stick is set, the right stick
// to give instead of the player's. ctx: wheel_zoom_context's (WZ_NONE drops what was waiting); frame: the presented
// frame count, the pulses' clock (the game samples its controllers once a frame).
typedef struct {
    uint64_t buttons;
    bool stick;
    int32_t rx, ry;
} WheelZoomPulse;
WheelZoomPulse wheel_zoom_read(int ctx, uint64_t frame);

#ifdef __cplusplus
}
#endif
