// SDL_GetError and the timer of the kept code. The audio device is runtime/ps5/audio.c, the window and events
// runtime/ps5/window.c, the input devices runtime/ps5/input.c.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdl_shim.h"
#include "../platform.h"
#include <stdatomic.h>

static _Atomic(const char *) g_error = "";
void ps5_sdl_error(const char *text) { atomic_store(&g_error, text); }
const char *SDL_GetError(void) { return atomic_load(&g_error); }

uint64_t SDL_GetTicks(void) { return plat_now_ns() / 1000000ull; }
