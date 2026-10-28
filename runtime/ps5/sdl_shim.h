// The SDL calls the kept runtime code still makes on the PS5 (D2: a symbol needs a caller). The window is the
// VideoOut display plane of runtime/ps5/window.c; there is no audio device (M5) and no input device (M4).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct SDL_Window SDL_Window;

#define SDLCALL
#define SDL_INIT_AUDIO 0x00000010u
typedef uint32_t SDL_AudioDeviceID;
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID)0xffffffffu)
typedef enum { SDL_AUDIO_S16 = 0x8010u, SDL_AUDIO_F32 = 0x8120u } SDL_AudioFormat;
typedef struct { SDL_AudioFormat format; int channels; int freq; } SDL_AudioSpec;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef void (*SDL_AudioStreamCallback)(void *, SDL_AudioStream *, int, int);
bool SDL_InitSubSystem(uint32_t flags);
const char *SDL_GetError(void);
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID, const SDL_AudioSpec *, SDL_AudioStreamCallback, void *);
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *);
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID, SDL_AudioSpec *, int *);
float SDL_GetAudioStreamGain(SDL_AudioStream *);
bool SDL_SetAudioStreamGain(SDL_AudioStream *, float);
int SDL_GetAudioStreamQueued(SDL_AudioStream *);
bool SDL_PutAudioStreamData(SDL_AudioStream *, const void *, int);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *);
bool SDL_SetAudioStreamGetCallback(SDL_AudioStream *, SDL_AudioStreamCallback, void *);
void SDL_DestroyAudioStream(SDL_AudioStream *);

typedef struct SDL_Gamepad SDL_Gamepad;
uint64_t SDL_GetTicks(void);
bool SDL_RumbleGamepad(SDL_Gamepad *, uint16_t low, uint16_t high, uint32_t milliseconds);

// ---- video (runtime/ps5/window.c)
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef uint32_t SDL_DisplayID;
typedef uint32_t SDL_PropertiesID;
typedef uint64_t SDL_WindowFlags;
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_INIT_GAMEPAD 0x00002000u
#define SDL_WINDOW_FULLSCREEN 0x0000000000000001ull
#define SDL_WINDOW_HIDDEN 0x0000000000000008ull
#define SDL_WINDOW_MINIMIZED 0x0000000000000040ull
#define SDL_WINDOW_RESIZABLE 0x0000000000000020ull
#define SDL_WINDOW_HIGH_PIXEL_DENSITY 0x0000000000002000ull
#define SDL_WINDOW_VULKAN 0x0000000010000000ull
#define SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"
#define SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN "SDL.window.HDR_enabled"
#define SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT "SDL.window.SDR_white_level"
typedef struct {
    SDL_DisplayID displayID;
    uint32_t format;
    int w, h;
    float pixel_density;
    float refresh_rate;
    int refresh_rate_numerator, refresh_rate_denominator;
    void *internal;
} SDL_DisplayMode;
bool SDL_Init(uint32_t flags);
bool SDL_SetHint(const char *name, const char *value);
SDL_DisplayID SDL_GetPrimaryDisplay(void);
const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID display);
SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags);
bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h);
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window);
const SDL_DisplayMode *SDL_GetWindowFullscreenMode(SDL_Window *window);
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *window);
bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value);
float SDL_GetFloatProperty(SDL_PropertiesID props, const char *name, float default_value);

// ---- events: the display plane has no window events, and no input device exists before M4
#define SDL_EVENT_QUIT 0x100u
typedef union SDL_Event { uint32_t type; uint8_t padding[128]; } SDL_Event;
bool SDL_WaitEventTimeout(SDL_Event *event, int32_t timeout_ms);
bool SDL_PollEvent(SDL_Event *event);
bool SDL_PushEvent(SDL_Event *event);

// ---- timer and CPU
uint64_t SDL_GetTicksNS(void);
void SDL_Delay(uint32_t ms);
void SDL_DelayPrecise(uint64_t ns);
int SDL_GetNumLogicalCPUCores(void);
int SDL_GetSystemRAM(void);
