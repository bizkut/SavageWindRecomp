// PS5 inline system keyboard for nn::swkbd::ShowKeyboard.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../rt.h"
#include "sdl_shim.h"
#include "../keyboard_utf.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// libSceIme's PS4-compatible parameter layout, as used by Forever's inline IME.
typedef struct {
    int32_t user;
    uint32_t type;
    uint64_t languages;
    uint32_t enter_label, input_method;
    void *filter;
    uint32_t option, max_length;
    uint16_t *text;
    float x, y;
    uint32_t halign, valign;
    void *work, *arg;
    void (*handler)(void *, const void *);
    uint8_t reserved[8];
} ImeParam;
_Static_assert(sizeof(ImeParam) == 0x60, "IME parameter ABI");
_Static_assert(offsetof(ImeParam, text) == 0x28, "IME text ABI");
_Static_assert(offsetof(ImeParam, work) == 0x40, "IME work ABI");
_Static_assert(offsetof(ImeParam, handler) == 0x50, "IME handler ABI");

int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t, const void *, int *);
const char *sceKernelGetFsSandboxRandomWord(void);
int32_t sceUserServiceGetInitialUser(int32_t *user_id);
void sceImeParamInit(ImeParam *param);
int sceImeOpen(const ImeParam *param, const void *extended);
int sceImeUpdate(void (*handler)(void *, const void *));
int sceImeClose(void);
void backend_represent(void);
void input_reset(void);

// Public OrbisImeEventId values; candidate-list events starting at 7 are not terminal.
enum { IME_MAX_TEXT = 2048, EVENT_PRESS_CLOSE = 4, EVENT_PRESS_ENTER = 5, EVENT_ABORT = 6 };
static uint8_t work[0x40000] __attribute__((aligned(0x4000)));
static uint16_t text[IME_MAX_TEXT + 1];
static int keyboard_open, ime_loaded;

int ps5_keyboard_is_open(void) {
    return __atomic_load_n(&keyboard_open, __ATOMIC_ACQUIRE);
}

static void on_ime_event(void *arg, const void *event) {
    int *result = arg;
    uint32_t id = *(const uint32_t *)event;
    int decision;
    switch (id) {
    case EVENT_PRESS_CLOSE:
    case EVENT_ABORT: decision = -1; break;
    case EVENT_PRESS_ENTER: decision = 1; break;
    default: return;
    }
    // One terminal diagnostic per session, and the first terminal event wins.
    if (!*result) {
        *result = decision;
        LOG("[swkbd] IME event %u: %s\n", id, decision > 0 ? "confirm" : "cancel");
    }
}

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    if (!out || cap <= 0) return 0;
    int32_t user = -1;
    int rc = sceUserServiceGetInitialUser(&user);
    if (rc < 0 || user < 0) {
        LOG("[swkbd] initial user unavailable %#x (user %d)\n", (unsigned)rc, user);
        return 0;
    }
    if (!ime_loaded) {
        char path[128];
        const char *word = sceKernelGetFsSandboxRandomWord();
        int status = 0;
        snprintf(path, sizeof(path), "/%s/common/lib/libSceIme.sprx", word ? word : "system");
        rc = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &status);
        if (rc < 0) {
            LOG("[swkbd] IME module load failed %#x (status %#x)\n", (unsigned)rc, (unsigned)status);
            return 0;
        }
        ime_loaded = 1;
    }
    int limit = max_len > 0 ? max_len : 8;
    if (limit > IME_MAX_TEXT) limit = IME_MAX_TEXT;
    memset(text, 0, sizeof(text));
    keyboard_utf8_to_utf16(initial, text, (size_t)limit + 1, (size_t)limit);
    int result = 0;
    ImeParam param;
    sceImeParamInit(&param);
    param.user = user;
    param.max_length = (uint32_t)limit;
    param.text = text;
    param.x = 960.0f;
    param.y = 540.0f;
    param.halign = 1;
    param.valign = 1;
    param.work = work;
    param.arg = &result;
    param.handler = on_ime_event;
    input_reset();
    __atomic_store_n(&keyboard_open, 1, __ATOMIC_RELEASE);
    rc = sceImeOpen(&param, NULL);
    if (rc != 0) {
        LOG("[swkbd] IME open failed %#x\n", (unsigned)rc);
    } else {
        while (!result && !g_rt_unwind) {
            uint64_t start = SDL_GetTicks();
            rc = sceImeUpdate(on_ime_event);
            if (rc != 0) {
                LOG("[swkbd] IME update failed %#x\n", (unsigned)rc);
                result = -1;
                break;
            }
            backend_represent();
            uint64_t elapsed = SDL_GetTicks() - start;
            if (elapsed < 16) SDL_Delay((uint32_t)(16 - elapsed));
        }
        if (g_rt_unwind) result = -1;
        if (result == 1) {
            keyboard_utf16_to_utf8(text, (size_t)limit + 1, out, (size_t)cap, (size_t)limit);
        }
        rc = sceImeClose();
        if (rc != 0) LOG("[swkbd] IME close failed %#x\n", (unsigned)rc);
    }
    memset(text, 0, sizeof(text));
    __atomic_store_n(&keyboard_open, 0, __ATOMIC_RELEASE);
    input_reset();
    return result == 1;
}
