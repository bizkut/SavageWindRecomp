// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include "../runtime/ps5/video_defaults.h"

int main(void) {
    Settings s = {0};
    assert(video_defaults_present(NULL) == 0);
    assert(video_defaults_apply(&s, 0, 3840, 2160, 119880));
    assert(s.frame_gen && s.upscaler == UPSCALE_FSR1 && s.upscale_quality == UPQ_PERFORMANCE);
    s = (Settings){0};
    assert(video_defaults_apply(&s, 0, 3840, 2160, 60000));
    assert(!s.frame_gen && s.upscaler == UPSCALE_FSR1 && s.upscale_quality == UPQ_QUALITY);
    s = (Settings){0};
    assert(video_defaults_apply(&s, 0, 1920, 1080, 119880));
    assert(s.frame_gen && s.upscaler == UPSCALE_OFF);
    FILE *f = tmpfile();
    assert(f);
    fputs("frame_generation = 0\nupscaler = off\nupscaler_quality = balanced\nwindow_size = default\nframe_rate = 30\n", f);
    rewind(f);
    unsigned present = video_defaults_present(f);
    fclose(f);
    s = (Settings){.frame_gen = false, .upscaler = UPSCALE_OFF, .upscale_quality = UPQ_BALANCED, .fps_limit = 30};
    assert(present == VIDEO_HAS_ALL);
    assert(!video_defaults_apply(&s, present, 3840, 2160, 119880));
    assert(!s.frame_gen && s.upscaler == UPSCALE_OFF && s.upscale_quality == UPQ_BALANCED && s.fps_limit == 30);
    s = (Settings){.frame_gen = false, .upscaler = UPSCALE_FSR1, .upscale_quality = UPQ_ULTRA};
    assert(video_defaults_apply(&s, VIDEO_HAS_FG | VIDEO_HAS_UPSCALER | VIDEO_HAS_QUALITY, 1920, 1080, 119880));
    assert(!s.frame_gen && s.upscaler == UPSCALE_FSR1 && s.upscale_quality == UPQ_ULTRA);
    s = (Settings){.frame_gen = false};
    assert(video_defaults_apply(&s, VIDEO_HAS_FG, 3840, 2160, 119880));
    assert(!s.frame_gen && s.upscaler == UPSCALE_FSR1 && s.upscale_quality == UPQ_QUALITY);
    return 0;
}
