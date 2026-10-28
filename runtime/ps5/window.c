// The renderer's window on the PS5: VideoOut's display plane (docs/PS5-PORT-PLAN.md D4). runtime/nvn/vulkan/device.c
// makes its window, Vulkan surface and swapchain through SDL calls; here they are the console's one display, a
// VK_KHR_display plane surface. Its mode is chosen from what the display reports, never assumed: the largest extent,
// at 119.88 Hz where that extent has it (RADV's VideoOut WSI offers 119.88 Hz only where the title's metadata
// declares it and the TV takes it, ps5/FINDINGS.md S4). The swapchain, and with it the internal resolution
// (runtime/nvn/output_scale.c), follow that extent; only a 4K TV (.41) has been measured. Presents are FIFO
// (Video > VSync On): VideoOut flips at vblank, and with VRR the TV follows the presents between 48 and 120 Hz. Also
// the output-mode half of runtime/window_size.c (one size: the display's), the timer and the event queue the kept code
// polls (a quit request, and the input devices' events from runtime/ps5/input.c).
// SPDX-License-Identifier: GPL-3.0-or-later
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../paths.h"
#include "../platform.h"
#include "../settings.h"
#include "../window_size.h"

#define HIGH_REFRESH_MHZ 119880

int64_t sceKernelGetDirectMemorySize(void);  // libkernel

struct SDL_Window { int w, h; };  // the chosen mode's extent (0 x 0 until the surface is made)
static SDL_Window g_window;
static SDL_DisplayMode g_mode = {.displayID = 1, .pixel_density = 1};
static atomic_bool g_window_made, g_quit;
static _Atomic float g_hz;  // the display plane's refresh rate (0 until the surface is made)

// ---- SDL video
bool SDL_Init(uint32_t flags) {
    (void)flags;
    return true;
}
bool SDL_SetHint(const char *name, const char *value) {
    (void)name; (void)value;
    return true;
}
SDL_DisplayID SDL_GetPrimaryDisplay(void) { return g_mode.displayID; }
const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID display) {
    return display == g_mode.displayID ? &g_mode : NULL;
}

SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags) {
    (void)title; (void)flags;
    if (atomic_exchange(&g_window_made, true)) {
        ps5_logf("[ps5] window: the display plane is already the window\n");
        return NULL;
    }
    // FIFO: VideoOut flips at vblank whatever the mode asked (its WSI), and framepace.c keeps the frame rate just
    // under the refresh rate with VSync On, which keeps a VRR TV inside its range (D4)
    if (g_settings.vsync != VSYNC_ON)
        ps5_logf("[ps5] Video > VSync set to On (FIFO) for VideoOut, was %d\n", g_settings.vsync);
    g_settings.vsync = VSYNC_ON;
    ps5_logf("[ps5] window: VideoOut display plane, its size from the display mode (asked %dx%d), no SDL video\n", w, h);
    ps5_logf("[ps5] Video page (savagewind.ini): frame rate %d (0: off), upscaler %d, upscaler mode %d, anti-aliasing "
             "%d, field of view %d%%\n", g_settings.fps_limit, g_settings.upscaler, g_settings.upscale_quality,
             g_settings.antialias, g_settings.fov);
    return &g_window;
}

bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h) {
    if (!window) return false;
    *w = window->w;
    *h = window->h;
    return true;
}
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window) {
    return window ? SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN : 0;
}
// the display plane has its own mode: "exclusive fullscreen" in device.c's swapchain line
const SDL_DisplayMode *SDL_GetWindowFullscreenMode(SDL_Window *window) { return window ? &g_mode : NULL; }
// HDR (device.c hdr_wanted): the WSI offers SDR formats only; Video > HDR On still asks and logs that it is SDR
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *window) { return window ? 1 : 0; }
bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value) {
    (void)props; (void)name;
    return default_value;
}
float SDL_GetFloatProperty(SDL_PropertiesID props, const char *name, float default_value) {
    (void)props; (void)name;
    return default_value;
}
void rt_window_icon(SDL_Window *w) { (void)w; }  // (a title's icon is sce_sys/icon0.png)

// ---- the Vulkan surface: VK_KHR_display on VideoOut
const char *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count) {
    static const char *const ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    *count = 2;
    return ext;
}

// attribute3 of the title's param.json, the metadata VideoOut's high-frame-rate and VRR answers depend on (-1: unread)
static long title_attribute3(void) {
    FILE *f = fopen("/app0/sce_sys/param.json", "rb");
    if (!f) return -1;
    char text[16384];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    const char *at = strstr(text, "\"attribute3\"");
    if (!at || !(at = strchr(at, ':'))) return -1;
    return strtol(at + 1, NULL, 0);
}

// Which of two modes to take for a wanted extent (0 x 0: Auto, the largest): that extent when the display has it, else
// the larger; at the same extent, the refresh rate nearest 119.88 Hz. The wanted extent is Video > Resolution
// (g_settings.window_w / window_h, savagewind.ini window_size; runtime/ps5/video_page.cpp), read when the surface is
// made: a choice applies at the next start.
static bool mode_better(const VkDisplayModeParametersKHR *a, const VkDisplayModeParametersKHR *b, WinSize want) {
    bool wa = a->visibleRegion.width == (uint32_t)want.w && a->visibleRegion.height == (uint32_t)want.h;
    bool wb = b->visibleRegion.width == (uint32_t)want.w && b->visibleRegion.height == (uint32_t)want.h;
    if (wa != wb) return wa;
    uint64_t aa = (uint64_t)a->visibleRegion.width * a->visibleRegion.height;
    uint64_t ab = (uint64_t)b->visibleRegion.width * b->visibleRegion.height;
    if (aa != ab) return aa > ab;
    long da = labs((long)a->refreshRate - HIGH_REFRESH_MHZ), db = labs((long)b->refreshRate - HIGH_REFRESH_MHZ);
    return da < db;
}
// The extents the display reports (distinct, largest first): Video > Resolution offers 1080p and 4K among them
static WinSize g_reported[16];
static atomic_int g_nreported;
static void note_reported(const VkDisplayModeParametersKHR *m) {
    WinSize s = {(int)m->visibleRegion.width, (int)m->visibleRegion.height};
    int n = atomic_load(&g_nreported);
    for (int i = 0; i < n; i++)
        if (g_reported[i].w == s.w && g_reported[i].h == s.h) return;
    if (n == 16) return;
    int at = n;
    while (at > 0 && (long long)g_reported[at - 1].w * g_reported[at - 1].h < (long long)s.w * s.h) {
        g_reported[at] = g_reported[at - 1];
        at--;
    }
    g_reported[at] = s;
    atomic_store(&g_nreported, n + 1);
}

bool SDL_Vulkan_CreateSurface(SDL_Window *window, VkInstance instance, const VkAllocationCallbacks *allocator,
                              VkSurfaceKHR *surface) {
    if (window != &g_window) return false;
    uint32_t npd = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkResult r = vkEnumeratePhysicalDevices(instance, &npd, &pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !npd) {
        ps5_logf("[ps5] VideoOut: no Vulkan device (%d)\n", r);
        return false;
    }
    uint32_t nd = 1;
    VkDisplayPropertiesKHR dp;
    r = vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &nd, &dp);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !nd) {
        ps5_logf("[ps5] VideoOut: no display (%d)\n", r);
        return false;
    }
    uint32_t nm = 0;
    vkGetDisplayModePropertiesKHR(pd, dp.display, &nm, NULL);
    VkDisplayModePropertiesKHR modes[16];
    if (nm > 16) nm = 16;
    vkGetDisplayModePropertiesKHR(pd, dp.display, &nm, modes);
    ps5_logf("[ps5] VideoOut: display \"%s\" %ux%u, %u modes\n", dp.displayName ? dp.displayName : "?",
             dp.physicalResolution.width, dp.physicalResolution.height, nm);
    WinSize want = {g_settings.window_w, g_settings.window_h};
    bool want_reported = false;
    int best = -1;
    for (uint32_t i = 0; i < nm; i++) {
        const VkDisplayModeParametersKHR *m = &modes[i].parameters;
        ps5_logf("[ps5] VideoOut: mode %u: %ux%u at %.3f Hz\n", i, m->visibleRegion.width, m->visibleRegion.height,
                 m->refreshRate / 1000.0);
        if (!m->visibleRegion.width || !m->visibleRegion.height) continue;
        note_reported(m);
        want_reported |= m->visibleRegion.width == (uint32_t)want.w && m->visibleRegion.height == (uint32_t)want.h;
        if (best < 0 || mode_better(m, &modes[best].parameters, want)) best = (int)i;
    }
    if (want.w > 0 && want.h > 0)
        ps5_logf("[ps5] Video > Resolution: %dx%d (savagewind.ini)%s\n", want.w, want.h,
                 want_reported ? "" : ", not a mode this display reports: Auto");
    else
        ps5_logf("[ps5] Video > Resolution: Auto (the largest mode)\n");
    if (best < 0) {
        ps5_logf("[ps5] VideoOut: the display reports no mode\n");
        return false;
    }
    const VkDisplayModePropertiesKHR *mode = &modes[best];
    VkDisplaySurfaceCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
                                        .displayMode = mode->displayMode, .planeIndex = 0,
                                        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .globalAlpha = 1.0f,
                                        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
                                        .imageExtent = mode->parameters.visibleRegion};
    r = vkCreateDisplayPlaneSurfaceKHR(instance, &ci, allocator, surface);
    if (r != VK_SUCCESS) {
        ps5_logf("[ps5] VideoOut: vkCreateDisplayPlaneSurfaceKHR(mode %d) failed: %d\n", best, r);
        return false;
    }
    float hz = mode->parameters.refreshRate / 1000.0f;
    window->w = (int)mode->parameters.visibleRegion.width;
    window->h = (int)mode->parameters.visibleRegion.height;
    g_mode.w = window->w;
    g_mode.h = window->h;
    g_mode.refresh_rate = hz;
    g_mode.refresh_rate_numerator = (int)mode->parameters.refreshRate;
    g_mode.refresh_rate_denominator = 1000;
    atomic_store(&g_hz, hz);
    long a3 = title_attribute3();
    ps5_logf("[ps5] VideoOut: chose mode %d: display plane surface %dx%d at %.3f Hz, present mode FIFO\n", best,
             window->w, window->h, hz);
    ps5_logf("[ps5] VideoOut: %s (param.json attribute3 %#lx: VRR %s, high frame rate %s)\n",
             mode->parameters.refreshRate >= 119000 ? "119.88 Hz; the TV follows the presents with VRR (48-120 Hz)"
                                                    : "no 119.88 Hz mode offered (TV, console 120 Hz setting)",
             a3, a3 >= 0 && (a3 & 0x40000) && !(a3 & 0x80000) ? "on" : "off",
             a3 >= 0 && (a3 & 0x40) ? "declared" : "not declared");
    return true;
}

// ---- output modes (runtime/window_size.h): the display plane's one size, the chosen mode's (0 x 0 before)
static WinSize display_size(void) { return (WinSize){g_mode.w, g_mode.h}; }
const char *winsize_aspect(int w, int h) { return h > 0 && (long long)w * 9 == (long long)h * 16 ? "16:9" : NULL; }
// The sizes offered: the extents the display reports, largest first (the surface's own until it is made)
int winsize_list(WinSize *out, bool fullscreen) {
    (void)fullscreen;
    int n = atomic_load(&g_nreported);
    for (int i = 0; i < n; i++) out[i] = g_reported[i];
    if (n) return n;
    out[0] = display_size();
    return 1;
}
WinSize winsize_effective(bool fullscreen) { (void)fullscreen; return display_size(); }
WinSize winsize_default(bool fullscreen) { (void)fullscreen; return display_size(); }
WinSize winsize_desktop(void) { return display_size(); }
float winsize_refresh_rate(void) { return atomic_load(&g_hz); }
void winsize_name(WinSize s, const char *native, char *buf, int n) {
    const char *a = winsize_aspect(s.w, s.h);
    if (native) snprintf(buf, (size_t)n, "%d x %d  (%s%s%s)", s.w, s.h, a ? a : "", a ? ", " : "", native);
    else snprintf(buf, (size_t)n, "%d x %d", s.w, s.h);
}
bool winsize_virtual(int *w, int *h) { (void)w; (void)h; return false; }
void winsize_refresh(void *sdl_window) { (void)sdl_window; }
void winsize_apply(void *sdl_window) {
    (void)sdl_window;
    ps5_logf("[window] VideoOut display plane, fullscreen in the display's mode (Video > Resolution applies at the next "
             "start)\n");
}

// ---- events: a quit request (game_options.mm Quit) and the input devices' events (runtime/ps5/input.c, read as the
// window loop asks for events). A quit never waits behind a full queue.
#define EVENT_QUEUE 1024
static pthread_mutex_t g_events_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_Event g_events[EVENT_QUEUE];
static unsigned g_events_head, g_events_count;
bool SDL_PushEvent(SDL_Event *event) {
    if (event->type == SDL_EVENT_QUIT) {
        atomic_store(&g_quit, true);
        return true;
    }
    pthread_mutex_lock(&g_events_lock);
    bool room = g_events_count < EVENT_QUEUE;
    if (room) g_events[(g_events_head + g_events_count++) % EVENT_QUEUE] = *event;
    pthread_mutex_unlock(&g_events_lock);
    if (!room) {
        static uint64_t said_ns;
        uint64_t now = plat_now_ns();
        if (now - said_ns > 1000000000ull) {
            said_ns = now;
            ps5_logf("[ps5] event queue full (%d events): event %#x dropped\n", EVENT_QUEUE, event->type);
        }
    }
    return room;
}
bool SDL_PollEvent(SDL_Event *event) {
    if (atomic_exchange(&g_quit, false)) {  // (the window loop would exit(); a title closes through the system instead)
        ps5_logf("[ps5] quit requested: the title closes\n");
        ps5_exit_title();
    }
    ps5_input_poll();
    pthread_mutex_lock(&g_events_lock);
    bool any = g_events_count > 0;
    if (any) {
        *event = g_events[g_events_head];
        g_events_head = (g_events_head + 1) % EVENT_QUEUE;
        g_events_count--;
    }
    pthread_mutex_unlock(&g_events_lock);
    return any;
}
bool SDL_WaitEventTimeout(SDL_Event *event, int32_t timeout_ms) {
    uint64_t until = plat_now_ns() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000000ull;
    for (;;) {
        if (SDL_PollEvent(event)) return true;
        uint64_t now = plat_now_ns();
        if (now >= until) return false;
        uint64_t step = until - now < 2000000ull ? until - now : 2000000ull;
        usleep((useconds_t)(step / 1000));
    }
}

// ---- timer and CPU
uint64_t SDL_GetTicksNS(void) { return plat_now_ns(); }
void SDL_Delay(uint32_t ms) { usleep((useconds_t)ms * 1000); }
void SDL_DelayPrecise(uint64_t ns) { plat_sleep_until_ns(plat_now_ns() + ns); }
int SDL_GetNumLogicalCPUCores(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
int SDL_GetSystemRAM(void) { return (int)(sceKernelGetDirectMemorySize() >> 20); }
