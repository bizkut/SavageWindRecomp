// Host test: runtime/ps5/window.c takes the display's largest mode, at 119.88 Hz where that size has it, logs every
// reported mode and the one chosen, and sizes the window (the swapchain) from it; nothing assumes 3840x2160.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/ps5/window.c"
#include <assert.h>
#include <math.h>
#include <stdarg.h>

Settings g_settings = {.vsync = VSYNC_FAST};
static char logbuf[8192];
void ps5_logf(const char *format, ...) {
    size_t used = strlen(logbuf);
    va_list args;
    va_start(args, format);
    vsnprintf(logbuf + used, sizeof logbuf - used, format, args);
    va_end(args);
}
uint64_t plat_now_ns(void) { return 0; }
void plat_sleep_until_ns(uint64_t t) { (void)t; }
int64_t sceKernelGetDirectMemorySize(void) { return 12ll << 30; }

static VkDisplayModeParametersKHR g_modes[8];
static uint32_t g_nmodes;
static VkDisplayModeKHR g_created_mode;
#define MODE_HANDLE(i) ((VkDisplayModeKHR)(uintptr_t)(0x100 + (i)))
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance i, uint32_t *n, VkPhysicalDevice *pd) {
    (void)i;
    *n = 1;
    *pd = (VkPhysicalDevice)(uintptr_t)0x10;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice pd, uint32_t *n,
                                                                       VkDisplayPropertiesKHR *p) {
    (void)pd;
    *n = 1;
    *p = (VkDisplayPropertiesKHR){.display = (VkDisplayKHR)(uintptr_t)0x20, .displayName = "PS5 VideoOut",
                                  .physicalResolution = {3840, 2160}};
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetDisplayModePropertiesKHR(VkPhysicalDevice pd, VkDisplayKHR d, uint32_t *n,
                                                             VkDisplayModePropertiesKHR *p) {
    (void)pd; (void)d;
    if (!p) { *n = g_nmodes; return VK_SUCCESS; }
    if (*n > g_nmodes) *n = g_nmodes;
    for (uint32_t i = 0; i < *n; i++) p[i] = (VkDisplayModePropertiesKHR){MODE_HANDLE(i), g_modes[i]};
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDisplayPlaneSurfaceKHR(VkInstance i, const VkDisplaySurfaceCreateInfoKHR *ci,
                                                              const VkAllocationCallbacks *a, VkSurfaceKHR *s) {
    (void)i; (void)a;
    g_created_mode = ci->displayMode;
    *s = (VkSurfaceKHR)(uintptr_t)0x30;
    return VK_SUCCESS;
}

static void choose(SDL_Window *w, const VkDisplayModeParametersKHR *modes, uint32_t n, int want, int ww, int wh,
                   float hz) {
    memcpy(g_modes, modes, n * sizeof *modes);
    g_nmodes = n;
    logbuf[0] = 0;
    g_created_mode = VK_NULL_HANDLE;
    VkSurfaceKHR s = VK_NULL_HANDLE;
    assert(SDL_Vulkan_CreateSurface(w, (VkInstance)(uintptr_t)1, NULL, &s) == (want >= 0));
    if (want < 0) {
        assert(strstr(logbuf, "no mode"));
        return;
    }
    assert(g_created_mode == MODE_HANDLE(want));
    int pw = 0, ph = 0;
    assert(SDL_GetWindowSizeInPixels(w, &pw, &ph) && pw == ww && ph == wh);
    assert(winsize_desktop().w == ww && winsize_desktop().h == wh && fabsf(winsize_refresh_rate() - hz) < 0.01f);
    char line[128];
    for (uint32_t i = 0; i < n; i++) {
        snprintf(line, sizeof line, "mode %u: %ux%u at %.3f Hz", i, modes[i].visibleRegion.width,
                 modes[i].visibleRegion.height, modes[i].refreshRate / 1000.0);
        assert(strstr(logbuf, line));
    }
    snprintf(line, sizeof line, "chose mode %d: display plane surface %dx%d at %.3f Hz", want, ww, wh, hz);
    assert(strstr(logbuf, line));
}

int main(void) {
    SDL_Window *w = SDL_CreateWindow("Savage Wind", 1280, 720, SDL_WINDOW_VULKAN);
    assert(w && g_settings.vsync == VSYNC_ON);
    assert(!SDL_CreateWindow("again", 1, 1, 0));
    int pw = -1, ph = -1;
    assert(SDL_GetWindowSizeInPixels(w, &pw, &ph) && pw == 0 && ph == 0);  // (no size before the mode)
    const VkDisplayModeParametersKHR uhd[] = {{{3840, 2160}, 59940}, {{3840, 2160}, 119880}, {{1920, 1080}, 119880}};
    choose(w, uhd, 3, 1, 3840, 2160, 119.88f);
    const VkDisplayModeParametersKHR fhd[] = {{{1920, 1080}, 119880}, {{1920, 1080}, 59940}};
    choose(w, fhd, 2, 0, 1920, 1080, 119.88f);
    const VkDisplayModeParametersKHR qhd[] = {{{1920, 1080}, 59940}, {{2560, 1440}, 59940}};
    choose(w, qhd, 2, 1, 2560, 1440, 59.94f);
    choose(w, NULL, 0, -1, 0, 0, 0);
    SDL_Event e;
    assert(!SDL_PollEvent(&e));
    e.type = SDL_EVENT_QUIT;
    assert(SDL_PushEvent(&e) && SDL_WaitEventTimeout(&e, 16) && e.type == SDL_EVENT_QUIT && !SDL_PollEvent(&e));
    return 0;
}
