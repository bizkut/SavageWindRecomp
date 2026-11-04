// Host test: clang -std=c11 -Wall -Wextra -o /tmp/test_ps5_keyboard tests/test_ps5_keyboard.c && /tmp/test_ps5_keyboard
#include <assert.h>
#include <string.h>
#include "../runtime/ps5/keyboard.c"

volatile int g_rt_unwind;
static ImeParam saved;
static int event_id, second_event = -1, closes, resets, frames, updates, fail_update;
int sceKernelLoadStartModule(const char *path, size_t n, const void *p, uint32_t f, const void *o, int *status) {
    (void)n; (void)p; (void)f; (void)o; (void)status;
    assert(strcmp(path, "/sandbox/common/lib/libSceIme.sprx") == 0); return 1;
}
const char *sceKernelGetFsSandboxRandomWord(void) { return "sandbox"; }
int32_t sceUserServiceGetInitialUser(int32_t *user) { *user = 42; return 0; }
void sceImeParamInit(ImeParam *p) { memset(p, 0, sizeof(*p)); }
int sceImeOpen(const ImeParam *p, const void *ext) {
    assert(!ext && ps5_keyboard_is_open());
    assert(p->user == 42 && p->max_length == 3 && p->work && p->handler);
    assert(p->text[0] == 'a' && p->text[1] == 0);
    saved = *p; return 0;
}
int sceImeUpdate(void (*handler)(void *, const void *)) {
    assert(ps5_keyboard_is_open()); updates++;
    if (fail_update) return -1;
    if (updates == 1) {
        uint32_t candidate_list_start = 7;
        handler(saved.arg, &candidate_list_start); // not a terminal event
        return 0;
    }
    saved.text[0] = 'b'; saved.text[1] = 0;
    handler(saved.arg, &event_id);
    if (second_event >= 0) handler(saved.arg, &second_event);
    return 0;
}
int sceImeClose(void) { assert(ps5_keyboard_is_open()); closes++; return 0; }
void input_reset(void) { resets++; }
void backend_represent(void) { frames++; }
uint64_t SDL_GetTicks(void) { return 0; }
void SDL_Delay(uint32_t ms) { (void)ms; }
int main(void) {
    char out[16] = "unchanged";
    // Public event IDs: 4 press close, 5 press enter, 6 abort.
    event_id = 4; second_event = 5;
    assert(!menu_keyboard_run("a", 3, out, sizeof(out)));
    assert(!strcmp(out, "unchanged") && closes == 1 && resets == 2 && frames == 2);
    assert(!ps5_keyboard_is_open());
    updates = 0; event_id = 5; second_event = 4;
    assert(menu_keyboard_run("a", 3, out, sizeof(out)) && !strcmp(out, "b"));
    assert(closes == 2 && resets == 4);
    updates = 0; event_id = 6; second_event = -1;
    snprintf(out, sizeof out, "unchanged");
    assert(!menu_keyboard_run("a", 3, out, sizeof(out)) && !strcmp(out, "unchanged"));
    assert(closes == 3 && resets == 6);
    updates = 0; fail_update = 1;
    assert(!menu_keyboard_run("a", 3, out, sizeof(out)) && closes == 4);
    fail_update = 0; g_rt_unwind = 1;
    assert(!menu_keyboard_run("a", 3, out, sizeof(out)) && closes == 5);
    assert(!ps5_keyboard_is_open());
    return 0;
}
