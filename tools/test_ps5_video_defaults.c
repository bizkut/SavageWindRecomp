// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include "../runtime/ps5/video_defaults.h"

int main(void) {
    Settings s = {0};
    assert(video_defaults_present(NULL) == 0);
    assert(video_output_parse("4k") == 1);
    assert(video_output_parse("1080p") == 2);
    assert(video_output_parse("") == 0);
    assert(video_output_parse("invalid") == 0);
    assert(video_output_parse("auto") == VIDEO_OUTPUT_AUTO);
    assert(video_output_parse("720p") == VIDEO_OUTPUT_720P);
    assert(video_output_parse("1440p") == VIDEO_OUTPUT_1440P);
    assert(video_output_from_mode(2) == VIDEO_OUTPUT_720P);
    assert(video_output_from_mode(0) == VIDEO_OUTPUT_1080P);
    assert(video_output_from_mode(6) == VIDEO_OUTPUT_1440P);
    assert(video_output_from_mode(19) == VIDEO_OUTPUT_4K);
    assert(video_output_from_mode(-1) == VIDEO_OUTPUT_4K);
    const int outputs[] = {VIDEO_OUTPUT_720P, VIDEO_OUTPUT_1080P, VIDEO_OUTPUT_1440P, VIDEO_OUTPUT_4K};
    const int widths[] = {1280, 1920, 2560, 3840};
    const int heights[] = {720, 1080, 1440, 2160};
    for (int i = 0; i < 4; i++) {
        int w, h;
        video_output_extent(outputs[i], &w, &h);
        assert(w == widths[i] && h == heights[i]);
        assert(video_output_parse(video_output_name(outputs[i])) == outputs[i]);
    }
    assert(video_defaults_apply(&s, 0, 3840, 2160, 119880, VIDEO_HDR_CAPABLE));
    assert(s.frame_gen && s.upscaler == UPSCALE_FSR3 && s.upscale_quality == UPQ_PERFORMANCE && s.hdr == HDR_ON &&
           s.sharp_ui && s.hd_ui_generate);
    s = (Settings){0};
    assert(video_defaults_apply(&s, 0, 3840, 2160, 60000, VIDEO_HDR_NOT_CAPABLE));
    assert(!s.frame_gen && s.upscaler == UPSCALE_FSR3 && s.upscale_quality == UPQ_QUALITY &&
           s.hdr == HDR_OFF && s.sharp_ui && s.hd_ui_generate);
    s = (Settings){0};
    assert(video_defaults_apply(&s, 0, 1920, 1080, 119880, VIDEO_HDR_UNKNOWN));
    assert(s.frame_gen && s.upscaler == UPSCALE_OFF && s.upscale_quality == UPQ_PERFORMANCE &&
           s.hdr == HDR_OFF && !s.sharp_ui && !s.hd_ui_generate);
    s = (Settings){0};
    assert(video_defaults_apply(&s, 0, 1920, 1080, 60000, VIDEO_HDR_NOT_CAPABLE));
    assert(!s.frame_gen && s.upscaler == UPSCALE_OFF && s.upscale_quality == UPQ_QUALITY &&
           !s.sharp_ui && !s.hd_ui_generate);
    for (int i = 0; i < 2; i++) {
        s = (Settings){0};
        assert(video_defaults_apply(&s, 0, widths[i * 2], heights[i * 2], 119880, VIDEO_HDR_CAPABLE));
        assert(s.frame_gen && s.upscaler == UPSCALE_OFF && s.upscale_quality == UPQ_PERFORMANCE &&
               s.hdr == HDR_ON && !s.sharp_ui && !s.hd_ui_generate);
    }
    FILE *f = tmpfile();
    assert(f);
    fputs("frame_generation = 0\nupscaler = off\nupscaler_quality = balanced\nwindow_size = default\nframe_rate = 30\nhdr = off\n"
          "sharp_ui = 0\nhd_ui_generate = 0\n", f);
    rewind(f);
    unsigned present = video_defaults_present(f);
    fclose(f);
    s = (Settings){.frame_gen = false, .upscaler = UPSCALE_OFF, .upscale_quality = UPQ_BALANCED, .fps_limit = 30};
    assert(present == VIDEO_HAS_ALL);
    assert(!video_defaults_apply(&s, present, 3840, 2160, 119880, VIDEO_HDR_CAPABLE));
    assert(!s.frame_gen && s.upscaler == UPSCALE_OFF && s.upscale_quality == UPQ_BALANCED && s.fps_limit == 30 &&
           !s.sharp_ui && !s.hd_ui_generate);
    s = (Settings){.frame_gen = false, .upscaler = UPSCALE_FSR1, .upscale_quality = UPQ_ULTRA};
    assert(video_defaults_apply(&s, VIDEO_HAS_FG | VIDEO_HAS_UPSCALER | VIDEO_HAS_QUALITY, 1920, 1080, 119880, VIDEO_HDR_UNKNOWN));
    assert(!s.frame_gen && s.upscaler == UPSCALE_FSR1 && s.upscale_quality == UPQ_ULTRA);
    s = (Settings){.frame_gen = false};
    assert(video_defaults_apply(&s, VIDEO_HAS_FG, 3840, 2160, 119880, VIDEO_HDR_UNKNOWN));
    assert(!s.frame_gen && s.upscaler == UPSCALE_FSR3 && s.upscale_quality == UPQ_QUALITY);
    s.hdr = HDR_ON;
    assert(video_defaults_apply(&s, VIDEO_HAS_HDR, 3840, 2160, 119880, VIDEO_HDR_NOT_CAPABLE));
    assert(s.hdr == HDR_ON);
    s.hdr = HDR_OFF;
    assert(video_defaults_apply(&s, VIDEO_HAS_HDR, 3840, 2160, 119880, VIDEO_HDR_CAPABLE));
    assert(s.hdr == HDR_OFF);
    return 0;
}
