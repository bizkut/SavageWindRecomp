// SDL calls retained by the M2 runtime; no window or audio device is created.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Surface SDL_Surface;
#define SDL_MESSAGEBOX_ERROR 0x00000010u
#define SDL_PIXELFORMAT_RGBA32 0u
bool SDL_ShowSimpleMessageBox(uint32_t flags, const char *title, const char *message, SDL_Window *window);
SDL_Surface *SDL_CreateSurfaceFrom(int width, int height, uint32_t format, void *pixels, int pitch);
bool SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *surface);
void SDL_DestroySurface(SDL_Surface *surface);

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
