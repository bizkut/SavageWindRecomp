// First-launch PS5 video defaults, applied after VideoOut chooses its mode.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdio.h>
#include <string.h>
#include "../settings.h"

enum { VIDEO_HAS_FG = 1, VIDEO_HAS_UPSCALER = 2, VIDEO_HAS_QUALITY = 4,
       VIDEO_HAS_SIZE = 8, VIDEO_HAS_FPS = 16, VIDEO_HAS_HDR = 32, VIDEO_HAS_SHARP_UI = 64, VIDEO_HAS_ALL = 127 };
typedef enum { VIDEO_HDR_UNKNOWN, VIDEO_HDR_NOT_CAPABLE, VIDEO_HDR_CAPABLE } VideoHdrCapability;

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
        else if (!strcmp(key, "hdr")) present |= VIDEO_HAS_HDR;
        else if (!strcmp(key, "sharp_ui")) present |= VIDEO_HAS_SHARP_UI;
    }
    return present;
}

static bool video_defaults_apply(Settings *s, unsigned present, int width, int height, unsigned refresh_mhz,
                                 VideoHdrCapability hdr) {
    if (!(present & VIDEO_HAS_FG)) s->frame_gen = refresh_mhz >= 119000;
    if (!(present & VIDEO_HAS_HDR)) s->hdr = hdr == VIDEO_HDR_CAPABLE ? HDR_ON : HDR_OFF;
    // Layout-art enlargement only pays off when the output is 4K (user decision 2026-10-28).
    if (!(present & VIDEO_HAS_SHARP_UI)) s->sharp_ui = width >= 3840 && height >= 2160;
    if ((present & VIDEO_HAS_ALL) == VIDEO_HAS_ALL) return false;
    if (!(present & VIDEO_HAS_UPSCALER))
        s->upscaler = width >= 3840 && height >= 2160 ? UPSCALE_FSR1 : UPSCALE_OFF;
    if (!(present & VIDEO_HAS_QUALITY))
        s->upscale_quality = s->frame_gen ? UPQ_PERFORMANCE : UPQ_QUALITY;
    return true;
}
