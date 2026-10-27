// Host test for the M2 no-audio/no-controller device boundary.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/ps5/sdl_shim.h"
#include <assert.h>
#include <stdarg.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int logs;
void ps5_logf(const char *format, ...) { (void)format; logs++; }
uint64_t plat_now_ns(void) { return 1234000000ull; }
int main(void) {
    assert(!SDL_InitSubSystem(SDL_INIT_AUDIO));
    assert(!SDL_InitSubSystem(SDL_INIT_AUDIO));
    assert(!SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL, NULL, NULL));
    assert(!SDL_GetAudioDeviceFormat(0, NULL, NULL));
    assert(strstr(SDL_GetError(), "unavailable"));
    assert(logs == 1);
    assert(SDL_GetTicks() == 1234);
    assert(!SDL_RumbleGamepad(NULL, 0, 0, 0));
    assert(!SDL_RumbleGamepad(NULL, 0, 0, 0));
    assert(logs == 2);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        struct rlimit no_core = {0, 0}; setrlimit(RLIMIT_CORE, &no_core);
        SDL_GetAudioStreamQueued(NULL);
        _exit(0);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    return 0;
}
