// Motion controls (runtime/hle/motion.c): the game's six-axis sensor (gyroscope + accelerometer) from the controller's
// own sensors, or from the mouse while the tilt key (bindings: BIND_TILT) is held.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Main thread (runtime/hle/input.c). Sensor readings are in SDL's controller axes: X right, Y up out of its face, Z
// toward the player.
void motion_pad_opened(bool sensors);  // a controller opened; sensors: its gyroscope and accelerometer report
void motion_pad_closed(void);
void motion_accel(const float d[3]);                  // m/s^2, the push against gravity
void motion_gyro(const float d[3], uint64_t ts_ns);   // radians per second; the reading's sensor time
void motion_tilt_held(bool held);              // the tilt key, pressed or let go
void motion_mouse(float dx, float dy);         // mouse motion while the tilt key is held (pixels)

// Scripted runs (SAVAGEWIND_INPUT: TILT_UP / TILT_DOWN / TILT_LEFT / TILT_RIGHT, TURN_UP / TURN_DOWN / TURN_LEFT /
// TURN_RIGHT): a held tilt in degrees and turning speeds in degrees per second, this frame
void motion_script(float pitch_deg, float roll_deg, float pitch_speed, float yaw_speed);

bool motion_has_sensors(void);  // the controller has a gyroscope (else the mouse tilt is the only motion)

#ifdef __cplusplus
}
#endif
