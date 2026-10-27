// M2 has no window, interactive dialogs or host audio device.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdl_shim.h"
#include "../platform.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

bool SDL_ShowSimpleMessageBox(uint32_t flags, const char *title, const char *message, SDL_Window *window) {
    (void)flags; (void)window;
    fprintf(stderr, "[ps5] %s: %s\n", title, message);
    return false;
}
SDL_Surface *SDL_CreateSurfaceFrom(int width, int height, uint32_t format, void *pixels, int pitch) {
    (void)width; (void)height; (void)format; (void)pixels; (void)pitch;
    fprintf(stderr, "[ps5] window surfaces are unavailable in M2\n");
    abort();
}
bool SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *surface) {
    (void)window; (void)surface;
    fprintf(stderr, "[ps5] window icons are unavailable in M2\n");
    abort();
}
void SDL_DestroySurface(SDL_Surface *surface) {
    (void)surface;
    fprintf(stderr, "[ps5] window surfaces are unavailable in M2\n");
    abort();
}

static atomic_flag audio_reported = ATOMIC_FLAG_INIT;
static void audio_unavailable(void) {
    if (!atomic_flag_test_and_set(&audio_reported))
        ps5_logf("[audio] PS5 audio device unavailable in M2; renderer uses its no-device path\n");
}
bool SDL_InitSubSystem(uint32_t flags) { (void)flags; audio_unavailable(); return false; }
const char *SDL_GetError(void) { return "PS5 audio or controller device unavailable in M2"; }
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID id, const SDL_AudioSpec *spec,
                                          SDL_AudioStreamCallback callback, void *data) {
    (void)id; (void)spec; (void)callback; (void)data; audio_unavailable(); return NULL;
}
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID id, SDL_AudioSpec *spec, int *frames) {
    (void)id; (void)spec; (void)frames; audio_unavailable(); return false;
}
static _Noreturn void invalid_audio_stream(void) {
    ps5_logf("[audio] unexpected audio stream in M2\n");
    abort();
}
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *stream) { (void)stream; invalid_audio_stream(); }
float SDL_GetAudioStreamGain(SDL_AudioStream *stream) { (void)stream; invalid_audio_stream(); }
bool SDL_SetAudioStreamGain(SDL_AudioStream *stream, float gain) { (void)stream; (void)gain; invalid_audio_stream(); }
int SDL_GetAudioStreamQueued(SDL_AudioStream *stream) { (void)stream; invalid_audio_stream(); }
bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *data, int n) {
    (void)stream; (void)data; (void)n; invalid_audio_stream();
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream) { (void)stream; invalid_audio_stream(); }
bool SDL_SetAudioStreamGetCallback(SDL_AudioStream *stream, SDL_AudioStreamCallback callback, void *data) {
    (void)stream; (void)callback; (void)data; invalid_audio_stream();
}
void SDL_DestroyAudioStream(SDL_AudioStream *stream) { (void)stream; invalid_audio_stream(); }

uint64_t SDL_GetTicks(void) { return plat_now_ns() / 1000000ull; }
bool SDL_RumbleGamepad(SDL_Gamepad *pad, uint16_t low, uint16_t high, uint32_t milliseconds) {
    (void)pad; (void)low; (void)high; (void)milliseconds;
    static atomic_flag reported = ATOMIC_FLAG_INIT;
    if (!atomic_flag_test_and_set(&reported)) ps5_logf("[input] controller rumble unavailable in M2\n");
    return false;
}
