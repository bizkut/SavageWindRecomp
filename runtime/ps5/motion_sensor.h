// Convert scePad motion samples to SDL3 controller sensor events.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sdl_shim.h"
#include <string.h>

static inline SDL_Event ps5_motion_sensor_event(SDL_JoystickID id, SDL_SensorType sensor, const float data[3],
                                                uint64_t timestamp_us) {
    SDL_Event event;
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_GAMEPAD_SENSOR_UPDATE;
    event.gsensor.which = id;
    event.gsensor.sensor = sensor;
    // scePad's controller axes match SDL's: X right, Y out of the face, Z toward the player.
    // scePad acceleration is in G; SDL expects m/s². Both use rad/s for angular velocity.
    float scale = sensor == SDL_SENSOR_ACCEL ? SDL_STANDARD_GRAVITY : 1.0f;
    for (int i = 0; i < 3; i++) event.gsensor.data[i] = data[i] * scale;
    event.gsensor.sensor_timestamp = timestamp_us * 1000ull;
    return event;
}
