// The PS5's audio device behind the SDL audio-stream calls runtime/services/audio.c makes (D2, D9): one SceAudioOut
// main port at 48 kHz, stereo S16, fed by one worker thread, as PS5_vkQuake's src/audio_ps5.cpp (read for the AudioOut
// calls and their order; no code taken). Each SDL stream is a ring of stereo float frames the audio service fills; every
// AudioOut period (AUDIO_GRAIN frames) the worker takes up to a period from each resumed stream, mixes them with their
// gains, and blocks in sceAudioOutOutput, which paces it. The device is opened on the first SDL_InitSubSystem(AUDIO) and
// stays open for the process; streams come and go (the SDK renderer's, and an IAudioOut's per movie).
//
// Streams are 48 kHz only (the console's AudioOut rate; the game's renderer and audout both ask for it), S16 or F32,
// 1, 2 or 6 channels (6: FL FR FC LFE BL BR, folded to stereo as the renderer's device sink is). Another rate is refused
// and the audio service takes its no-device path, so nothing plays resampled by accident.
//
// The klog: one line when the device opens or fails, then every 10 s the window's counters: underruns (a stream that
// had data ran short in a period: silence was played for its missing frames), AudioOut errors (with the last result),
// frames dropped because a stream's ring was full, the deepest queue seen, and the level at each stage, so a silent
// output shows where the samples became zero: what the streams were given (SDL_PutAudioStreamData), what the mix read
// back from their rings, the gains of the resumed streams, and what went to AudioOut.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdl_shim.h"
#include "../platform.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// ---- the console's AudioOut (no SDK header: declarations as PS5_vkQuake's)
int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(int32_t user_id, int32_t port_type, int32_t index, uint32_t frames, uint32_t rate,
                        uint32_t format);
int32_t sceAudioOutOutput(int32_t port, const void *pcm);

#define AUDIOOUT_ALREADY_INITIALIZED 0x8026000eu
#define AUDIOOUT_USER_SYSTEM 0xff  // (the system user: the title's output, whoever is signed in)
#define AUDIOOUT_PORT_MAIN 0
#define AUDIOOUT_S16_STEREO 1

#define AUDIO_RATE 48000
#define AUDIO_GRAIN 256                         // frames per sceAudioOutOutput
#define AUDIO_PERIOD 1024                       // the device period streams are told (SDL3's default at 48 kHz)
#define AUDIO_RING (AUDIO_RATE / 2)             // frames a stream can hold: 0.5 s
#define AUDIO_STREAMS 8
#define AUDIO_WINDOW_NS 10000000000ull          // the counters' klog line
#define AUDIO_DEVICE_ID ((SDL_AudioDeviceID)1)  // (SDL_GetAudioStreamDevice: the one device)

struct SDL_AudioStream {
    SDL_AudioSpec spec;
    int frame_bytes;   // one input frame
    float *ring;       // stereo frames
    uint32_t head, count;
    float gain;
    bool resumed;      // SDL_ResumeAudioStreamDevice (an SDL stream's device starts paused)
    bool primed;       // has had data: running short after that is an underrun
    SDL_AudioStreamCallback get;
    void *get_data;
};

static struct {
    pthread_once_t once;
    pthread_mutex_t lock;  // (recursive: a get callback may use its stream)
    int state;             // 0 not opened yet, 1 running, -1 failed
    int32_t port;
    SDL_AudioStream *streams[AUDIO_STREAMS];
    // the window's counters
    uint64_t window_start;
    uint32_t underruns, short_frames, errors, dropped, queue_peak;
    int32_t last_error;
    uint64_t periods;
    // Output level over the window (what actually went to AudioOut): peak |sample| and sum of squares for RMS, so a
    // klog shows whether the game is audible without listening. dBFS: 0 = full scale, below -90 = silence.
    uint32_t level_peak;
    double level_sumsq;
    uint64_t level_samples;
    // Where silence would come from, stage by stage: what the streams were given (SDL_PutAudioStreamData: calls,
    // frames, peak), what the mix read back from their rings before the gain.
    uint32_t puts;
    uint64_t in_frames;
    float in_peak, ring_peak;
} g = {.once = PTHREAD_ONCE_INIT};

static void lock_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g.lock, &attr);
    pthread_mutexattr_destroy(&attr);
}
static void lock(void) {
    pthread_once(&g.once, lock_init);
    pthread_mutex_lock(&g.lock);
}
static void unlock(void) { pthread_mutex_unlock(&g.lock); }

static float clamp1(float v) { return v < -1.f ? -1.f : v > 1.f ? 1.f : v; }

// One AudioOut period: up to `frames` frames of each resumed stream, mixed, as S16 stereo. lock held.
static void audio_mix(int16_t *out, uint32_t frames) {
    float mix[AUDIO_GRAIN * 2];
    memset(mix, 0, sizeof(float) * 2 * frames);
    for (int i = 0; i < AUDIO_STREAMS; i++) {
        SDL_AudioStream *s = g.streams[i];
        if (!s || !s->resumed) continue;
        uint32_t n = s->count < frames ? s->count : frames;
        for (uint32_t f = 0; f < n; f++) {
            const float *src = s->ring + 2 * ((s->head + f) % AUDIO_RING);
            g.ring_peak = fmaxf(g.ring_peak, fmaxf(fabsf(src[0]), fabsf(src[1])));
            mix[2 * f] += src[0] * s->gain;
            mix[2 * f + 1] += src[1] * s->gain;
        }
        s->head = (s->head + n) % AUDIO_RING;
        s->count -= n;
        if (n < frames && s->primed) {
            g.underruns++;
            g.short_frames += frames - n;
            int need = (int)(frames - n) * s->frame_bytes;
            if (s->get) s->get(s->get_data, s, need, need);
        }
    }
    for (uint32_t k = 0; k < 2 * frames; k++) {
        out[k] = (int16_t)(clamp1(mix[k]) * 32767.f);
        uint32_t a = (uint32_t)(out[k] < 0 ? -(int32_t)out[k] : out[k]);
        if (a > g.level_peak) g.level_peak = a;
        g.level_sumsq += (double)out[k] * out[k];
    }
    g.level_samples += 2 * frames;
}

static double dbfs(double peak, double full) { return peak > 0 ? 20 * log10(peak / full) : -120; }

// The window's counters, once per AUDIO_WINDOW_NS. lock held.
static void audio_window(uint64_t now) {
    if (!g.window_start) g.window_start = now;
    if (now - g.window_start < AUDIO_WINDOW_NS) return;
    int streams = 0, resumed = 0;
    float gain = -1;  // (the lowest gain of a resumed stream: 0 would silence it)
    for (int i = 0; i < AUDIO_STREAMS; i++) {
        const SDL_AudioStream *s = g.streams[i];
        if (!s) continue;
        streams++;
        if (!s->resumed) continue;
        resumed++;
        if (gain < 0 || s->gain < gain) gain = s->gain;
    }
    double rms = g.level_samples ? sqrt(g.level_sumsq / (double)g.level_samples) : 0;
    ps5_logf("[audio] AudioOut %.1f s: %llu periods, %u underruns (%u frames short), %u errors (last %#x), "
             "%u frames dropped, %d streams (%d resumed, lowest gain %.2f), queue peak %u frames; in %u puts, %llu "
             "frames, peak %.1f dBFS; ring read peak %.1f dBFS; level peak %.1f dBFS rms %.1f dBFS\n",
             (now - g.window_start) / 1e9, (unsigned long long)g.periods, g.underruns, g.short_frames, g.errors,
             (unsigned)g.last_error, g.dropped, streams, resumed, gain, g.queue_peak, g.puts,
             (unsigned long long)g.in_frames, dbfs(g.in_peak, 1), dbfs(g.ring_peak, 1), dbfs(g.level_peak, 32767),
             dbfs(rms, 32767));
    g.window_start = now;
    g.periods = 0;
    g.underruns = g.short_frames = g.errors = g.dropped = g.queue_peak = 0;
    g.last_error = 0;
    g.level_peak = 0;
    g.level_sumsq = 0;
    g.level_samples = 0;
    g.puts = 0;
    g.in_frames = 0;
    g.in_peak = g.ring_peak = 0;
}

// One period's bookkeeping after sceAudioOutOutput returned `result`. lock held.
static void audio_account(int32_t result, uint64_t now) {
    g.periods++;
    if (result < 0) {
        g.errors++;
        g.last_error = result;
    }
    audio_window(now);
}

static void *audio_worker(void *arg) {
    (void)arg;
    int16_t out[AUDIO_GRAIN * 2];
    for (;;) {
        lock();
        audio_mix(out, AUDIO_GRAIN);
        unlock();
        int32_t result = sceAudioOutOutput(g.port, out);  // (blocks until the port takes the next period)
        lock();
        audio_account(result, plat_now_ns());
        unlock();
    }
    return NULL;
}

static void audio_open_device(void) {
    int32_t r = sceAudioOutInit();
    if (r < 0 && (uint32_t)r != AUDIOOUT_ALREADY_INITIALIZED) {
        ps5_logf("[audio] sceAudioOutInit failed: %#x\n", (unsigned)r);
        ps5_sdl_error("sceAudioOutInit failed");
        g.state = -1;
        return;
    }
    g.port = sceAudioOutOpen(AUDIOOUT_USER_SYSTEM, AUDIOOUT_PORT_MAIN, 0, AUDIO_GRAIN, AUDIO_RATE, AUDIOOUT_S16_STEREO);
    if (g.port < 0) {
        ps5_logf("[audio] sceAudioOutOpen failed: %#x\n", (unsigned)g.port);
        ps5_sdl_error("sceAudioOutOpen failed");
        g.state = -1;
        return;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, audio_worker, NULL)) {
        ps5_logf("[audio] AudioOut worker thread failed to start\n");
        ps5_sdl_error("AudioOut worker thread failed to start");
        g.state = -1;
        return;
    }
    pthread_detach(thread);
    g.state = 1;
    ps5_logf("[audio] AudioOut port %d: %d Hz, stereo S16, %d frames per output, streams told %d-frame periods\n",
             g.port, AUDIO_RATE, AUDIO_GRAIN, AUDIO_PERIOD);
}

bool SDL_InitSubSystem(uint32_t flags) {
    if (flags != SDL_INIT_AUDIO) {
        ps5_sdl_error("only SDL_INIT_AUDIO is served on the PS5");
        return false;
    }
    lock();
    if (!g.state) audio_open_device();
    bool ok = g.state == 1;
    unlock();
    return ok;
}

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID id, const SDL_AudioSpec *spec,
                                          SDL_AudioStreamCallback callback, void *data) {
    (void)callback, (void)data;  // (the audio service passes none: it pushes data and reads the queue)
    if (id != SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK || !spec) {
        ps5_sdl_error("only the default playback device is served on the PS5");
        return NULL;
    }
    int sample = spec->format == SDL_AUDIO_S16 ? 2 : spec->format == SDL_AUDIO_F32 ? 4 : 0;
    if (!sample || (spec->channels != 1 && spec->channels != 2 && spec->channels != 6) || spec->freq != AUDIO_RATE) {
        ps5_logf("[audio] stream refused: format %#x, %d channels, %d Hz (AudioOut takes 48000 Hz S16/F32, 1/2/6 ch)\n",
                 (unsigned)spec->format, spec->channels, spec->freq);
        ps5_sdl_error("stream format not served by the PS5 AudioOut device");
        return NULL;
    }
    lock();
    SDL_AudioStream *s = NULL;
    int slot = -1;
    for (int i = 0; i < AUDIO_STREAMS && slot < 0; i++) if (!g.streams[i]) slot = i;
    if (g.state == 1 && slot >= 0 && (s = calloc(1, sizeof *s)) && (s->ring = malloc(sizeof(float) * 2 * AUDIO_RING))) {
        s->spec = *spec;
        s->frame_bytes = sample * spec->channels;
        s->gain = 1.f;
        g.streams[slot] = s;
        ps5_logf("[audio] stream %d opened: %s, %d channels\n", slot, sample == 2 ? "S16" : "F32", spec->channels);
    } else {
        if (s) free(s), s = NULL;
        ps5_sdl_error(g.state != 1 ? "PS5 AudioOut device not open" : "no free PS5 audio stream");
    }
    unlock();
    return s;
}

SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *stream) { return stream ? AUDIO_DEVICE_ID : 0; }

bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID id, SDL_AudioSpec *spec, int *frames) {
    if (id != AUDIO_DEVICE_ID || g.state != 1) {
        ps5_sdl_error("no such PS5 audio device");
        return false;
    }
    if (spec) *spec = (SDL_AudioSpec){SDL_AUDIO_S16, 2, AUDIO_RATE};
    if (frames) *frames = AUDIO_PERIOD;
    return true;
}

float SDL_GetAudioStreamGain(SDL_AudioStream *stream) {
    lock();
    float gain = stream ? stream->gain : -1.f;
    unlock();
    return gain;
}

bool SDL_SetAudioStreamGain(SDL_AudioStream *stream, float gain) {
    if (!stream || gain < 0) return false;
    lock();
    stream->gain = gain;
    unlock();
    return true;
}

int SDL_GetAudioStreamQueued(SDL_AudioStream *stream) {
    if (!stream) return -1;
    lock();
    int bytes = (int)stream->count * stream->frame_bytes;
    unlock();
    return bytes;
}

// One input frame as stereo floats.
static void to_stereo(const SDL_AudioStream *s, const uint8_t *in, float *lr) {
    float v[6];
    for (int c = 0; c < s->spec.channels; c++)
        v[c] = s->spec.format == SDL_AUDIO_S16 ? ((const int16_t *)(const void *)in)[c] / 32768.f
                                               : ((const float *)(const void *)in)[c];
    if (s->spec.channels == 1) {
        lr[0] = lr[1] = v[0];
    } else if (s->spec.channels == 2) {
        lr[0] = v[0], lr[1] = v[1];
    } else {  // FL FR FC LFE BL BR
        lr[0] = v[0] + 0.707f * v[2] + 0.251f * v[3] + 0.707f * v[4];
        lr[1] = v[1] + 0.707f * v[2] + 0.251f * v[3] + 0.707f * v[5];
    }
}

bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *data, int n) {
    if (!stream || n < 0 || (n && !data) || n % stream->frame_bytes) {
        ps5_sdl_error("audio data is not whole frames of the stream's format");
        return false;
    }
    uint32_t frames = (uint32_t)(n / stream->frame_bytes);
    lock();
    uint32_t room = AUDIO_RING - stream->count, take = frames < room ? frames : room;
    const uint8_t *in = data;
    for (uint32_t f = 0; f < take; f++) {
        float *lr = stream->ring + 2 * ((stream->head + stream->count + f) % AUDIO_RING);
        to_stereo(stream, in + (size_t)f * (size_t)stream->frame_bytes, lr);
        g.in_peak = fmaxf(g.in_peak, fmaxf(fabsf(lr[0]), fabsf(lr[1])));
    }
    stream->count += take;
    if (take) stream->primed = true;
    g.puts++;
    g.in_frames += frames;
    g.dropped += frames - take;
    if (stream->count > g.queue_peak) g.queue_peak = stream->count;
    unlock();
    return true;
}

bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream) {
    if (!stream) return false;
    lock();
    stream->resumed = true;
    unlock();
    return true;
}

bool SDL_SetAudioStreamGetCallback(SDL_AudioStream *stream, SDL_AudioStreamCallback callback, void *data) {
    if (!stream) return false;
    lock();
    stream->get = callback;
    stream->get_data = data;
    unlock();
    return true;
}

void SDL_DestroyAudioStream(SDL_AudioStream *stream) {
    if (!stream) return;
    lock();
    for (int i = 0; i < AUDIO_STREAMS; i++) {
        if (g.streams[i] != stream) continue;
        g.streams[i] = NULL;
        ps5_logf("[audio] stream %d closed\n", i);
    }
    unlock();
    free(stream->ring);
    free(stream);
}
