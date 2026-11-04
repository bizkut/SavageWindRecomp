// SPDX-License-Identifier: GPL-3.0-or-later
#include "../runtime/ps5/motion_sensor.h"
#include <assert.h>
#include <math.h>

int main(void) {
    const float accel[3] = {0.25f, 1.0f, -0.5f};
    const float gyro[3] = {-1.25f, 0.5f, 2.0f};
    SDL_Event a = ps5_motion_sensor_event(42, SDL_SENSOR_ACCEL, accel, 1234567);
    SDL_Event g = ps5_motion_sensor_event(42, SDL_SENSOR_GYRO, gyro, 1234567);
    assert(a.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE && a.gsensor.which == 42);
    assert(a.gsensor.sensor == SDL_SENSOR_ACCEL && g.gsensor.sensor == SDL_SENSOR_GYRO);
    assert(a.gsensor.sensor_timestamp == 1234567000ull && g.gsensor.sensor_timestamp == 1234567000ull);
    for (int i = 0; i < 3; i++) {
        assert(fabsf(a.gsensor.data[i] - accel[i] * 9.80665f) < 0.00001f);
        assert(g.gsensor.data[i] == gyro[i]);
    }
    return 0;
}
