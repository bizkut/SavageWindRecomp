// First-launch PS5 video defaults, applied after VideoOut chooses its mode.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdio.h>
#include <string.h>
#include "../settings.h"

enum { VIDEO_HAS_FG = 1, VIDEO_HAS_UPSCALER = 2, VIDEO_HAS_QUALITY = 4,
       VIDEO_HAS_SIZE = 8, VIDEO_HAS_FPS = 16, VIDEO_HAS_HDR = 32, VIDEO_HAS_SHARP_UI = 64,
       VIDEO_HAS_HD_UI_GENERATE = 128, VIDEO_HAS_ALL = 255 };
typedef enum { VIDEO_HDR_UNKNOWN, VIDEO_HDR_NOT_CAPABLE, VIDEO_HDR_CAPABLE } VideoHdrCapability;

enum { VIDEO_OUTPUT_AUTO, VIDEO_OUTPUT_4K, VIDEO_OUTPUT_1080P, VIDEO_OUTPUT_720P, VIDEO_OUTPUT_1440P };

static inline int video_output_parse(const char *value) {
    if (!strcmp(value, "4k")) return VIDEO_OUTPUT_4K;
    if (!strcmp(value, "1080p")) return VIDEO_OUTPUT_1080P;
    if (!strcmp(value, "720p")) return VIDEO_OUTPUT_720P;
    if (!strcmp(value, "1440p")) return VIDEO_OUTPUT_1440P;
    return VIDEO_OUTPUT_AUTO;
}

static inline const char *video_output_name(int output) {
    switch (output) {
    case VIDEO_OUTPUT_720P: return "720p";
    case VIDEO_OUTPUT_1080P: return "1080p";
    case VIDEO_OUTPUT_1440P: return "1440p";
    case VIDEO_OUTPUT_4K: return "4k";
    default: return "auto";
    }
}

static inline int video_output_from_mode(int mode) {
    switch (mode) {
    case 2: return VIDEO_OUTPUT_720P;
    case 0: return VIDEO_OUTPUT_1080P;
    case 6: return VIDEO_OUTPUT_1440P;
    default: return VIDEO_OUTPUT_4K;
    }
}

static inline void video_output_extent(int output, int *width, int *height) {
    switch (output) {
    case VIDEO_OUTPUT_720P:
        *width = 1280;
        *height = 720;
        break;
    case VIDEO_OUTPUT_1080P:
        *width = 1920;
        *height = 1080;
        break;
    case VIDEO_OUTPUT_1440P:
        *width = 2560;
        *height = 1440;
        break;
    default:
        *width = 3840;
        *height = 2160;
        break;
    }
}

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
        else if (!strcmp(key, "hd_ui_generate")) present |= VIDEO_HAS_HD_UI_GENERATE;
    }
    return present;
}

static bool video_defaults_apply(Settings *s, unsigned present, int width, int height, unsigned refresh_mhz,
                                 VideoHdrCapability hdr) {
    if (!(present & VIDEO_HAS_FG)) s->frame_gen = refresh_mhz >= 119000;
    if (!(present & VIDEO_HAS_HDR)) s->hdr = hdr == VIDEO_HDR_CAPABLE ? HDR_ON : HDR_OFF;
    // Layout-art enlargement only pays off when the output is 4K (user decision 2026-10-28).
    if (!(present & VIDEO_HAS_SHARP_UI)) s->sharp_ui = width >= 3840 && height >= 2160;
    if (!(present & VIDEO_HAS_HD_UI_GENERATE)) s->hd_ui_generate = width >= 3840 && height >= 2160;
    if ((present & VIDEO_HAS_ALL) == VIDEO_HAS_ALL) return false;
    if (!(present & VIDEO_HAS_UPSCALER)) // FSR 3.1 at 4K (user decision 2026-10-29); FSR 1 stays selectable
        s->upscaler = width >= 3840 && height >= 2160 ? UPSCALE_FSR3 : UPSCALE_OFF;
    if (!(present & VIDEO_HAS_QUALITY))
        s->upscale_quality = s->frame_gen ? UPQ_PERFORMANCE : UPQ_QUALITY;
    return true;
}
