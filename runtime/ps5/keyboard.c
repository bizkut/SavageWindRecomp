// PS5 system keyboard for nn::swkbd::ShowKeyboard.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../rt.h"
#include "sdl_shim.h"
#include <stddef.h>
#include "../keyboard_utf.h"
#include <stdint.h>

// The payload SDK supplies libSceImeDialog.so but no IME headers. This is the Orbis IME dialog ABI;
// the PS5 payload SDK exports the same entry points. Keep the reserved fields zero.
typedef struct {
    int32_t user_id;
    uint32_t type;
    uint64_t languages;
    uint32_t enter_label;
    uint32_t input_method;
    void *filter;
    uint32_t option;
    uint32_t max_text_length;
    uint16_t *input_text_buffer;
    float posx, posy;
    uint32_t horizontal_alignment, vertical_alignment;
    const uint16_t *placeholder;
    const uint16_t *title;
    uint8_t reserved[16];
} ImeDialogParam;
typedef struct {
    uint32_t end_status;
    uint8_t reserved[12];
} ImeDialogResult;
_Static_assert(sizeof(ImeDialogParam) == 0x60, "IME dialog parameter ABI");
_Static_assert(offsetof(ImeDialogParam, input_text_buffer) == 0x28, "IME dialog buffer ABI");
_Static_assert(offsetof(ImeDialogParam, title) == 0x48, "IME dialog title ABI");
_Static_assert(sizeof(ImeDialogResult) == 0x10, "IME dialog result ABI");

int32_t sceSysmoduleLoadModule(uint16_t id);
int32_t sceUserServiceGetInitialUser(int32_t *user_id);
int32_t sceImeDialogInit(const ImeDialogParam *param, const void *extended);
int32_t sceImeDialogGetStatus(void);
int32_t sceImeDialogGetResult(ImeDialogResult *result);
int32_t sceImeDialogAbort(void);
int32_t sceImeDialogTerm(void);
void backend_represent(void);
void input_reset(void);

enum { IME_DIALOG_MODULE = 0x0096, IME_MAX_TEXT = 2048, IME_FINISHED = 2, IME_OK = 0 };
static int keyboard_open;

int ps5_keyboard_is_open(void) {
    return __atomic_load_n(&keyboard_open, __ATOMIC_ACQUIRE);
}

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    static const uint16_t title[] = {'E', 'n', 't', 'e', 'r', ' ', 'n', 'a', 'm', 'e', 0};
    static const uint16_t placeholder[] = {0};
    uint16_t text[IME_MAX_TEXT + 1] = {0};
    ImeDialogParam param = {0};
    ImeDialogResult result = {0};
    int32_t user = -1;
    int32_t rc = sceSysmoduleLoadModule(IME_DIALOG_MODULE);
    // The module may already be loaded. The linked import remains callable in that case.
    if (rc < 0) LOG("[swkbd] IME dialog sysmodule load %#x\n", (unsigned)rc);
    rc = sceUserServiceGetInitialUser(&user);
    if (rc < 0 || user < 0 || !out || cap <= 0) {
        LOG("[swkbd] initial user unavailable %#x\n", (unsigned)rc);
        return 0;
    }
    int limit = max_len > 0 ? max_len : 8;
    if (limit > IME_MAX_TEXT) limit = IME_MAX_TEXT;
    keyboard_utf8_to_utf16(initial, text, (size_t)limit + 1, (size_t)limit);
    param.user_id = user;
    param.max_text_length = (uint32_t)limit;
    param.input_text_buffer = text;
    param.title = title;
    param.placeholder = placeholder;
    input_reset();
    __atomic_store_n(&keyboard_open, 1, __ATOMIC_RELEASE);
    rc = sceImeDialogInit(&param, NULL);
    if (rc < 0) {
        LOG("[swkbd] IME dialog init failed %#x\n", (unsigned)rc);
        __atomic_store_n(&keyboard_open, 0, __ATOMIC_RELEASE);
        input_reset();
        return 0;
    }
    int status, aborted = 0;
    while ((status = sceImeDialogGetStatus()) != IME_FINISHED && status >= 0) {
        if (g_rt_unwind && !aborted) {
            rc = sceImeDialogAbort();
            aborted = 1;
            if (rc < 0) {
                LOG("[swkbd] IME dialog abort failed %#x\n", (unsigned)rc);
                break;
            }
        }
        uint64_t start = SDL_GetTicks();
        backend_represent();
        uint64_t elapsed = SDL_GetTicks() - start;
        if (elapsed < 16) SDL_Delay((uint32_t)(16 - elapsed));
    }
    int ok = !aborted && status == IME_FINISHED && sceImeDialogGetResult(&result) >= 0 &&
             result.end_status == IME_OK;
    if (ok) {
        keyboard_utf16_to_utf8(text, (size_t)limit + 1, out, (size_t)cap, (size_t)limit);
        ok = out[0] != '\0';
    }
    rc = sceImeDialogTerm();
    if (rc < 0) LOG("[swkbd] IME dialog term failed %#x\n", (unsigned)rc);
    __atomic_store_n(&keyboard_open, 0, __ATOMIC_RELEASE);
    input_reset();
    return ok;
}
