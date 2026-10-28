// runtime/ps5/audio.c on the host, against a scripted SceAudioOut: the device's open and failure paths, the streams the
// audio service opens (formats, refusals, queue, gain, resume, 6-channel fold-down, mono), the period mix and its
// clamp, underruns and the get callback, a full ring, and the 10 s counters line. make test-ps5-audio
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void ps5_logf(const char *format, ...);
#include "../runtime/ps5/audio.c"

// ---- fakes: clock, errors, klog, the console's AudioOut
static uint64_t now_ns = 5000000000ull;
uint64_t plat_now_ns(void) { return now_ns; }
static const char *last_error = "";
void ps5_sdl_error(const char *text) { last_error = text; }
static char klog[64][512];
static int nklog;
void ps5_logf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    if (nklog < 64) vsnprintf(klog[nklog++], sizeof klog[0], format, ap);
    va_end(ap);
}
static bool logged(const char *text) {
    for (int i = 0; i < nklog; i++) if (strstr(klog[i], text)) return true;
    return false;
}

static int32_t init_result, open_result = 7;
static int32_t open_args[6];
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t out_cond = PTHREAD_COND_INITIALIZER;
static bool in_output;
int32_t sceAudioOutInit(void) { return init_result; }
int32_t sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t frames, uint32_t rate, uint32_t format) {
    int32_t a[6] = {user, type, index, (int32_t)frames, (int32_t)rate, (int32_t)format};
    memcpy(open_args, a, sizeof a);
    return open_result;
}
// The worker's first output never returns: the tests below drive audio_mix / audio_account themselves.
int32_t sceAudioOutOutput(int32_t port, const void *pcm) {
    assert(port == open_result && pcm);
    pthread_mutex_lock(&out_lock);
    in_output = true;
    pthread_cond_broadcast(&out_cond);
    for (;;) pthread_cond_wait(&out_cond, &out_lock);
}

static int callbacks, callback_bytes;
static SDL_AudioStream *callback_stream;
static void SDLCALL get_callback(void *u, SDL_AudioStream *st, int additional, int total) {
    assert(u == &callbacks && additional == total);
    callbacks++;
    callback_bytes += additional;
    callback_stream = st;
    assert(SDL_GetAudioStreamQueued(st) == 0);  // (the device lock is recursive: a callback may use its stream)
}

static SDL_AudioStream *open_stream(SDL_AudioFormat format, int channels, int freq) {
    SDL_AudioSpec spec = {format, channels, freq};
    return SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
}

int main(void) {
    int16_t out[AUDIO_GRAIN * 2];

    // ---- the device: a failed init is reported, the next try opens the port and starts the worker
    assert(!SDL_InitSubSystem(0x20));  // (not audio)
    init_result = (int32_t)0x80260001u;
    assert(!SDL_InitSubSystem(SDL_INIT_AUDIO));
    assert(strstr(last_error, "sceAudioOutInit") && logged("sceAudioOutInit failed: 0x80260001"));
    assert(!open_stream(SDL_AUDIO_F32, 2, 48000) && strstr(last_error, "not open"));
    g.state = 0;
    init_result = (int32_t)AUDIOOUT_ALREADY_INITIALIZED;
    assert(SDL_InitSubSystem(SDL_INIT_AUDIO));
    assert(SDL_InitSubSystem(SDL_INIT_AUDIO));  // (opened once)
    int32_t want[6] = {0xff, 0, 0, AUDIO_GRAIN, 48000, 1};
    assert(!memcmp(open_args, want, sizeof want));
    assert(logged("AudioOut port 7: 48000 Hz, stereo S16, 256 frames per output"));
    pthread_mutex_lock(&out_lock);
    while (!in_output) pthread_cond_wait(&out_cond, &out_lock);
    pthread_mutex_unlock(&out_lock);

    // ---- streams the AudioOut doesn't take are refused: the audio service then plays nothing (its no-device path)
    assert(!open_stream(SDL_AUDIO_F32, 2, 44100) && logged("44100 Hz"));
    assert(!open_stream(SDL_AUDIO_S16, 4, 48000));
    assert(!open_stream((SDL_AudioFormat)0x8008, 2, 48000));
    assert(!SDL_OpenAudioDeviceStream(3, &(SDL_AudioSpec){SDL_AUDIO_F32, 2, 48000}, NULL, NULL));

    // ---- the renderer's stream: stereo F32; paused until resumed, queue in input bytes, gain
    SDL_AudioStream *ren = open_stream(SDL_AUDIO_F32, 2, 48000);
    assert(ren);
    SDL_AudioSpec dev;
    int dev_frames = 0;
    assert(SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(ren), &dev, &dev_frames));
    assert(dev.freq == 48000 && dev.channels == 2 && dev.format == SDL_AUDIO_S16 && dev_frames == AUDIO_PERIOD);
    assert(!SDL_GetAudioDeviceFormat(9, &dev, &dev_frames));
    float f[AUDIO_GRAIN * 2];
    for (int i = 0; i < AUDIO_GRAIN; i++) f[2 * i] = 0.5f, f[2 * i + 1] = -0.25f;
    assert(SDL_PutAudioStreamData(ren, f, sizeof f));
    assert(SDL_GetAudioStreamQueued(ren) == (int)sizeof f);
    assert(!SDL_PutAudioStreamData(ren, f, 6));  // (not whole frames)
    lock();
    audio_mix(out, AUDIO_GRAIN);  // paused: silence, the data stays
    unlock();
    for (int i = 0; i < 2 * AUDIO_GRAIN; i++) assert(out[i] == 0);
    assert(SDL_GetAudioStreamQueued(ren) == (int)sizeof f);
    assert(SDL_ResumeAudioStreamDevice(ren));
    assert(SDL_GetAudioStreamGain(ren) == 1.f);
    assert(SDL_SetAudioStreamGain(ren, 0.5f) && SDL_GetAudioStreamGain(ren) == 0.5f);
    lock();
    audio_mix(out, AUDIO_GRAIN);
    unlock();
    assert(out[0] == (int16_t)(0.25f * 32767.f) && out[1] == (int16_t)(-0.125f * 32767.f));
    assert(out[2 * AUDIO_GRAIN - 2] == out[0] && out[2 * AUDIO_GRAIN - 1] == out[1]);
    assert(SDL_GetAudioStreamQueued(ren) == 0 && g.underruns == 0);

    // ---- underrun: a stream that had data runs short; the get callback is told the shortfall in input bytes
    assert(SDL_SetAudioStreamGetCallback(ren, get_callback, &callbacks));
    assert(SDL_PutAudioStreamData(ren, f, 100 * 2 * sizeof(float)));
    lock();
    audio_mix(out, AUDIO_GRAIN);
    unlock();
    assert(out[2 * 99] != 0 && out[2 * 100] == 0 && out[2 * AUDIO_GRAIN - 1] == 0);
    assert(g.underruns == 1 && g.short_frames == AUDIO_GRAIN - 100);
    assert(callbacks == 1 && callback_bytes == (AUDIO_GRAIN - 100) * 8 && callback_stream == ren);

    // ---- an IAudioOut's stream: 6-channel S16 folds to stereo; a stream with no data yet never underruns
    SDL_AudioStream *movie = open_stream(SDL_AUDIO_S16, 6, 48000);
    assert(movie && SDL_ResumeAudioStreamDevice(movie));
    assert(SDL_SetAudioStreamGain(ren, 1.f));
    lock();
    audio_mix(out, AUDIO_GRAIN);
    unlock();
    assert(g.underruns == 2 && callbacks == 2);  // (only the renderer's)
    int16_t six[AUDIO_GRAIN * 6];
    for (int i = 0; i < AUDIO_GRAIN; i++) {
        int16_t s[6] = {1000, 2000, 4000, 8000, 3000, -3000};
        memcpy(six + 6 * i, s, sizeof s);
    }
    assert(SDL_PutAudioStreamData(movie, six, sizeof six));
    assert(SDL_GetAudioStreamQueued(movie) == (int)sizeof six);
    lock();
    audio_mix(out, AUDIO_GRAIN);
    unlock();
    float l = (1000 + 0.707f * 4000 + 0.251f * 8000 + 0.707f * 3000) / 32768.f;
    float r = (2000 + 0.707f * 4000 + 0.251f * 8000 - 0.707f * 3000) / 32768.f;
    assert(out[0] == (int16_t)(l * 32767.f) && out[1] == (int16_t)(r * 32767.f));
    SDL_DestroyAudioStream(movie);
    assert(logged("stream 1 closed"));

    // ---- mono S16, and the mix clamps
    SDL_AudioStream *mono = open_stream(SDL_AUDIO_S16, 1, 48000);
    assert(mono && SDL_ResumeAudioStreamDevice(mono));
    int16_t loud[AUDIO_GRAIN];
    for (int i = 0; i < AUDIO_GRAIN; i++) loud[i] = 30000;
    assert(SDL_PutAudioStreamData(mono, loud, sizeof loud));
    for (int i = 0; i < AUDIO_GRAIN; i++) f[2 * i] = f[2 * i + 1] = 0.9f;
    assert(SDL_PutAudioStreamData(ren, f, sizeof f));
    lock();
    audio_mix(out, AUDIO_GRAIN);
    unlock();
    assert(out[0] == 32767 && out[1] == 32767);
    for (int i = 0; i < AUDIO_GRAIN; i++) loud[i] = -30000;
    assert(SDL_PutAudioStreamData(mono, loud, sizeof loud));
    lock();
    audio_mix(out, AUDIO_GRAIN);
    unlock();
    assert(out[0] == (int16_t)(-30000 / 32768.f * 32767.f) && out[0] == out[1]);

    // ---- a full ring drops what doesn't fit, and counts it
    static int16_t lots[AUDIO_RING + 10];
    assert(SDL_PutAudioStreamData(mono, lots, sizeof lots));
    assert(SDL_GetAudioStreamQueued(mono) == AUDIO_RING * 2 && g.dropped == 10 && g.queue_peak == AUDIO_RING);

    // ---- the counters' klog line every 10 s, with AudioOut errors; then a fresh window
    lock();
    audio_account(0, now_ns);
    audio_account((int32_t)0x80260008u, now_ns + 4000000000ull);
    assert(!logged("[audio] AudioOut 10"));
    audio_account(0, now_ns + 10000000000ull);
    unlock();
    assert(logged("[audio] AudioOut 10.0 s: 3 periods, 4 underruns (") && logged("1 errors (last 0x80260008), 10 frames dropped, 2 streams"));
    // the stages before the output: what the streams were given (the mono stream's 30000 the loudest), what the mix
    // read back from the rings before the gains, and the gains of the resumed streams
    assert(logged("2 streams (2 resumed, lowest gain 1.00), queue peak 24000 frames; in 7 puts, 25390 frames, "
                  "peak -0.8 dBFS; ring read peak -0.8 dBFS; level peak "));
    assert(g.underruns == 0 && g.errors == 0 && g.dropped == 0 && g.periods == 0);
    assert(g.puts == 0 && g.in_frames == 0 && g.in_peak == 0 && g.ring_peak == 0);

    // ---- at most AUDIO_STREAMS streams
    SDL_AudioStream *more[AUDIO_STREAMS];
    int n = 0;
    while (n < AUDIO_STREAMS && (more[n] = open_stream(SDL_AUDIO_F32, 2, 48000))) n++;
    assert(n == AUDIO_STREAMS - 2 && strstr(last_error, "no free"));
    for (int i = 0; i < n; i++) SDL_DestroyAudioStream(more[i]);
    SDL_DestroyAudioStream(mono);
    SDL_DestroyAudioStream(ren);
    for (int i = 0; i < AUDIO_STREAMS; i++) assert(!g.streams[i]);
    printf("test_ps5_audio: ok\n");
    return 0;
}
