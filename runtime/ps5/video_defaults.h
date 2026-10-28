// First-launch PS5 video defaults, applied after VideoOut chooses its mode.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdio.h>
#include <string.h>
#include "../settings.h"

enum { VIDEO_HAS_FG = 1, VIDEO_HAS_UPSCALER = 2, VIDEO_HAS_QUALITY = 4,
       VIDEO_HAS_SIZE = 8, VIDEO_HAS_FPS = 16, VIDEO_HAS_ALL = 31 };

static unsigned video_defaults_present(FILE *f) {
    unsigned present = 0;
    char line[256], key[64], val[128];
    while (f && fgets(line, sizeof line, f)) {
        if (sscanf(line, " %63[^= ] = %127[^\r\n]", key, val) < 1) continue;
        if (!strcmp(key, "frame_generation")) present |= VIDEO_HAS_FG;
        else if (!strcmp(key, "upscaler")) present |= VIDEO_HAS_UPSCALER;
        else if (!strcmp(key, "upscaler_quality")) present |= VIDEO_HAS_QUALITY;
        else if (!strcmp(key, "window_size")) present |= VIDEO_HAS_SIZE;
        else if (!strcmp(key, "frame_rate")) present |= VIDEO_HAS_FPS;
    }
    return present;
}

static bool video_defaults_apply(Settings *s, unsigned present, int width, int height, unsigned refresh_mhz) {
    if (!(present & VIDEO_HAS_FG)) s->frame_gen = refresh_mhz >= 119000;
    if ((present & VIDEO_HAS_ALL) == VIDEO_HAS_ALL) return false;
    if (!(present & VIDEO_HAS_UPSCALER))
        s->upscaler = width >= 3840 && height >= 2160 ? UPSCALE_FSR1 : UPSCALE_OFF;
    if (!(present & VIDEO_HAS_QUALITY))
        s->upscale_quality = s->frame_gen ? UPQ_PERFORMANCE : UPQ_QUALITY;
    return true;
}
