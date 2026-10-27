/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SAVAGEWIND_TITLE_PLATFORM_H
#define SAVAGEWIND_TITLE_PLATFORM_H

#include <stdint.h>

int sceUserServiceInitialize(const void *param);
int sceUserServiceGetInitialUser(int *user);
int scePadInit(void);
int scePadOpen(int user, int type, int index, const void *param);
int scePadRead(int handle, void *records, int count);
int sceSystemServiceLoadExec(const char *path, const char *const *argv);

/* PS5 pad record layout, as used by the ps5-wow title input implementation. */
struct pad_touch {
    uint16_t x, y;
    uint8_t id, reserved[3];
};

struct pad_record {
    uint32_t buttons;
    uint8_t sticks[8];
    float orientation[4], accel[3], gyro[3];
    uint8_t touch_count, reserved[7];
    struct pad_touch touch[2];
    int32_t connected;
    uint64_t timestamp;
    uint8_t extension[16], connected_count, reserved2[2], unique_length, unique[12];
};
_Static_assert(sizeof(struct pad_record) == 120, "pad record");

#endif
