// The SDL audio calls of the kept code: no host audio device (M5) yet. The window, timer and events are
// runtime/ps5/window.c; the input devices runtime/ps5/input.c.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdl_shim.h"
#include "../platform.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static _Atomic(const char *) g_error = "PS5 audio device unavailable before M5";
void ps5_sdl_error(const char *text) { atomic_store(&g_error, text); }
const char *SDL_GetError(void) { return atomic_load(&g_error); }

static atomic_flag audio_reported = ATOMIC_FLAG_INIT;
static void audio_unavailable(void) {
    if (!atomic_flag_test_and_set(&audio_reported))
        ps5_logf("[audio] PS5 audio device unavailable before M5; the audio service uses its no-device path\n");
}
bool SDL_InitSubSystem(uint32_t flags) {
    (void)flags;
    audio_unavailable();
    ps5_sdl_error("PS5 audio device unavailable before M5");
    return false;
}
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID id, const SDL_AudioSpec *spec,
                                          SDL_AudioStreamCallback callback, void *data) {
    (void)id; (void)spec; (void)callback; (void)data; audio_unavailable(); return NULL;
}
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID id, SDL_AudioSpec *spec, int *frames) {
    (void)id; (void)spec; (void)frames; audio_unavailable(); return false;
}
static _Noreturn void invalid_audio_stream(void) {
    ps5_logf("[audio] unexpected audio stream before M5\n");
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
