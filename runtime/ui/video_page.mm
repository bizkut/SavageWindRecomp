// The desktop's Video page of Savage Wind's Options pages (runtime/ui/game_options.mm): the window (fullscreen, size),
// the frame rate with VSync and HDR, the performance overlay, then the image rows every platform has, and AMD FSR 3
// frame generation (Vulkan). The PS5's page is runtime/ps5/video_page.cpp.
#include <math.h>
#include <stdio.h>

#include "game_options.h"
#include "i18n.h"

extern "C" {
#include "../settings.h"
#include "../window_size.h"
void menu_request_window_apply(void);  // menu.mm (fullscreen / window size, applied by the window loop)
}

namespace opts {
namespace {

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))
const char *const k_perf[] = {"Off", "FPS", "Full"};
#ifdef __APPLE__
const int k_ups_value[] = {UPSCALE_OFF, UPSCALE_METALFX};
#else
const int k_ups_value[] = {UPSCALE_OFF, UPSCALE_FSR1, UPSCALE_FSR3, UPSCALE_DLSS};
#endif
constexpr int k_fps[] = FPS_CHOICES;  // frame rate (0: Off, no limit)
const char *const k_vsync[VSYNC_COUNT] = {"On", "Fast", "Adaptive", "Off"};  // (settings.h VSYNC_*)
const char *const k_hdr[HDR_COUNT] = {"Automatic", "On", "Off"};               // (settings.h HDR_*)

// Window size: the sizes offered now (runtime/window_size.c; fullscreen can offer more)
int winsize_items(WinSize *list) { return winsize_list(list, g_settings.fullscreen); }
int winsize_index(WinSize s) {
    WinSize list[WINSIZE_MAX];
    int n = winsize_items(list);
    for (int i = 0; i < n; i++)
        if (list[i].w == s.w && list[i].h == s.h) return i;
    return 0;
}

}  // namespace

void video_page(std::vector<Item> &v) {
    // fullscreen and the window size are applied by the window loop (menu.mm)
    v.push_back(toggle(P_VIDEO, "Fullscreen", "Fills the screen. Also F11 or Alt+Enter.",
                       [](const Item &) { return (int)g_settings.fullscreen; },
                       [](const Item &, int x) { g_settings.fullscreen = x; menu_request_window_apply(); }, 0));
    // Window size: a button showing the size; A drops the sizes down as a list over it (menu.mm size_list_draw)
    Item ws = button(P_VIDEO, "Window size", "", nullptr, A_SIZE_LIST, 0);
    ws.label = [](const Item &, int) {
        static char buf[64];
        WinSize list[WINSIZE_MAX];
        int n = winsize_items(list), x = winsize_index(winsize_effective(g_settings.fullscreen));
        winsize_name(list[x < n ? x : 0], tr("native"), buf, sizeof buf);
        return (const char *)buf;
    };
    ws.desc_fn = [](const Item &) {
        static char buf[256];
        WinSize d = winsize_desktop();
        snprintf(buf, sizeof buf, tr("Window or fullscreen size. A: choose it from a list. Your display: %d x %d."), d.w,
                 d.h);
        return (const char *)buf;
    };
    ws.reset = [](const Item &) {  // (unset: 1280 x 720 in a window, the desktop in fullscreen)
        g_settings.window_w = g_settings.window_h = 0;
        menu_request_window_apply();
    };
    v.push_back(ws);
    v.push_back(video_wide_view());
    v.push_back(video_field_of_view());
    v.push_back(video_frame_rate(k_fps, N(k_fps)));
    // VSync (settings.h VSYNC_*; the present mode: runtime/nvn/vulkan/device.c), next to the frame rate it caps. (One
    // line for all its values: the game reads a row's description when the cursor comes to it, not when it changes.)
    v.push_back(choice(P_VIDEO, "VSync", "On suits G-SYNC and FreeSync. Fast: no tearing and no waiting.",
                       [](const Item &) { return (int)VSYNC_COUNT; },
                       [](const Item &, int x) { return tr(k_vsync[x]); },
                       [](const Item &) { return g_settings.vsync; },
                       [](const Item &, int x) { g_settings.vsync = x; }, VSYNC_DEFAULT));
    // HDR (settings.h HDR_*: runtime/nvn/vulkan/device.c hdr_wanted)
    v.push_back(choice(P_VIDEO, "HDR", "Automatic: in HDR while Windows HDR is on, as bright as in SDR. Off: always SDR.",
                       [](const Item &) { return (int)HDR_COUNT; },
                       [](const Item &, int x) { return tr(k_hdr[x]); },
                       [](const Item &) { return g_settings.hdr; },
                       [](const Item &, int x) { g_settings.hdr = x; }, HDR_AUTO));
    v.push_back(choice(P_VIDEO, "Performance overlay", "Frame rate and system load in a corner. Also F10.",
                       [](const Item &) { return N(k_perf); }, [](const Item &, int x) { return tr(k_perf[x]); },
                       [](const Item &) { return g_settings.perf_overlay; },
                       [](const Item &, int x) { g_settings.perf_overlay = x; }, PERF_OFF));
    v.push_back(video_upscaler(k_ups_value, N(k_ups_value)));
    v.push_back(video_upscaler_mode());
    v.push_back(video_antialiasing());
#ifndef __APPLE__
    v.push_back(video_anisotropy());
    v.push_back(video_sharpening());
    // AMD FSR 3 frame generation (vulkan/framegen.cpp)
    v.push_back(toggle(P_VIDEO, "Frame generation (AMD FSR 3)", "Twice the frames on screen. Adds a little delay.",
                       [](const Item &) { return (int)g_settings.frame_gen; },
                       [](const Item &, int x) { g_settings.frame_gen = x; }, 0));
#endif
    v.push_back(video_async_shaders());
    v.push_back(video_shadows());
}

// Quit: to the desktop (upper), or back to the start screen (lower: runtime/main.c reboots into it)
const QuitTexts k_quit_texts = {
    "To the desktop, or back to the Savage Wind start screen.",
    "Quit to the desktop, or back to the Savage Wind start screen? Unsaved progress is lost.",
    "Start screen",
    "Desktop",
};

}  // namespace opts
