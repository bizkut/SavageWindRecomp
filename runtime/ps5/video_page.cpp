// The PS5's Video page of Savage Wind's Options pages (runtime/ui/game_options.mm; docs/PS5-PORT-PLAN.md M4 "PS5 Video
// page"). Resolution: Auto / 1080p / 4K replaces the desktop's window rows; 1080p and 4K are offered only where the
// display reports them (runtime/ps5/window.c winsize_list). A choice applies at the next start: the display plane's
// surface is made once (window.c reads the setting then). Frame rate: 30 / 60 / Off, the desktop's frame_rate setting.
// The upscaler and its mode stay; VSync (FIFO is fixed, D4), HDR (not planned yet), the performance overlay and frame
// generation (M7) are left out. Choices persist in savagewind.ini through the storage broker (settings_save). Also the
// Quit question's texts: both its answers close the title.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>

#include "../ui/game_options.h"
#include "../ui/i18n.h"

extern "C" {
#include "../settings.h"
#include "../window_size.h"
}

namespace opts {
namespace {

const int k_fps[] = {30, 60, 0};                      // frame rate (0: Off, no limit)
const int k_ups_value[] = {UPSCALE_OFF, UPSCALE_FSR1};  // (the spatial upscaler; no temporal one on the PS5 yet)
const WinSize k_sizes[] = {{1920, 1080}, {3840, 2160}};
const char *const k_size_names[] = {"1080p", "4K"};

// Resolution's choices: Auto, then 1080p and 4K where the display reports them
int sizes_offered(WinSize *out, const char **names) {
    WinSize reported[WINSIZE_MAX];
    int nr = winsize_list(reported, true), n = 0;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < nr; i++)
            if (reported[i].w == k_sizes[k].w && reported[i].h == k_sizes[k].h) {
                if (out) out[n] = k_sizes[k];
                if (names) names[n] = k_size_names[k];
                n++;
                break;
            }
    return n;
}

}  // namespace

void video_page(std::vector<Item> &v) {
    Item res = choice(P_VIDEO, "Resolution", "",
                      [](const Item &) { return 1 + sizes_offered(nullptr, nullptr); },
                      [](const Item &, int x) {
                          const char *names[2];
                          sizes_offered(nullptr, names);
                          return x ? names[x - 1] : tr("Auto");
                      },
                      [](const Item &) {
                          WinSize s[2];
                          int n = sizes_offered(s, nullptr);
                          for (int i = 0; i < n; i++)
                              if (s[i].w == g_settings.window_w && s[i].h == g_settings.window_h) return i + 1;
                          return 0;
                      },
                      [](const Item &, int x) {
                          WinSize s[2];
                          int n = sizes_offered(s, nullptr);
                          WinSize w = x > 0 && x <= n ? s[x - 1] : WinSize{0, 0};
                          g_settings.window_w = w.w, g_settings.window_h = w.h;
                          fprintf(stderr, "[options] Video > Resolution: %s, at the next start\n",
                                  w.w ? (w.h == 2160 ? "4K" : "1080p") : "Auto");
                      },
                      0);
    res.desc_fn = [](const Item &) {  // (one line)
        static char buf[256];
        WinSize now = winsize_effective(true);
        snprintf(buf, sizeof buf, tr("Applies at the next start. Now: %d x %d at %.2f Hz."), now.w, now.h,
                 winsize_refresh_rate());
        return (const char *)buf;
    };
    v.push_back(res);
    v.push_back(video_frame_rate(k_fps, (int)(sizeof k_fps / sizeof k_fps[0])));
    v.push_back(video_wide_view());
    v.push_back(video_field_of_view());
    v.push_back(video_upscaler(k_ups_value, (int)(sizeof k_ups_value / sizeof k_ups_value[0])));
    v.push_back(video_upscaler_mode());
    v.push_back(video_antialiasing());
    v.push_back(video_anisotropy());
    v.push_back(video_sharpening());
    v.push_back(video_async_shaders());
    v.push_back(video_shadows());
}

// Quit: no start screen or desktop here; both answers close the title (runtime/platform/main_ps5.c), back to the
// PS5's home screen, where it starts again
const QuitTexts k_quit_texts = {
    "Closes the game, back to the PS5 home screen.",
    "Close Savage Wind and go back to the PS5 home screen? Unsaved progress is lost.",
    "Close game",
    "Home screen",
};

}  // namespace opts
