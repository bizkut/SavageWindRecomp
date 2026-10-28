// Boundaries for subsystems not linked into the PS5 title yet: button-glyph replacement, the overlay menu and Savage
// Wind's pages in the game's Options screen, frame generation (M7), movies (M5) and the desktop's performance overlay.
// Each answers as the desktop runtime does with no such device or feature.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../rt.h"
#include "../nvn/glyphs.h"
#include "../nvn/vulkan/framegen.h"
#include "../perfmon.h"
#include "../ui/menu.h"
#include <stdatomic.h>
#include <stdint.h>

const char *rt_version(void) { return "0.3.1-alpha.3"; }

// ---- the input devices are runtime/ps5/input.c behind runtime/hle/input.c and bindings.c (as on desktop)
bool bind_capture_event(const SDL_Event *e);  // runtime/hle/bindings.c
// Button prompts (runtime/nvn/glyphs.c draws the bound device's icons): the game's own glyphs
uint32_t glyphs_generation(void) { return 0; }
bool glyphs_apply(uint64_t addr, uint64_t pool_lo, uint64_t pool_hi, uint32_t format, uint32_t w, uint32_t h,
                  uint8_t *lin, uint32_t wb, uint32_t hb) {
    (void)addr; (void)pool_lo; (void)pool_hi; (void)format; (void)w; (void)h; (void)lin; (void)wb; (void)hb;
    return false;
}
bool glyphs_apply_layout(uint32_t format, uint32_t w, uint32_t h, uint8_t *lin, uint32_t row) {
    (void)format; (void)w; (void)h; (void)lin; (void)row;
    return false;
}

// ---- the overlay menu (runtime/ui/menu.mm, ImGui; opened with keys): none, and the display plane takes no requests.
// A binding capture (the game's Options screen) still takes the press first, as the menu's event handler does.
bool menu_handle_event(const SDL_Event *e) { return bind_capture_event(e); }
bool menu_is_open(void) { return false; }
bool menu_input_captured(void) { return false; }  // (no overlay or name entry has the keys)
bool menu_cursor_free(void) { return false; }     // (no Esc to free a cursor: the mouse always turns the camera)
void menu_apply_window_requests(void *window) { (void)window; }
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) { (void)cmd; (void)target_w; (void)target_h; }
void menu_test_open(void) {}

// ---- Savage Wind's pages in the game's Options screen and its Quit button (runtime/ui/game_options.mm): the game's
// own options and title menu. Each hook answers "the game's handling" (tools/patches.py).
void rt_opt_rows(void) {}
int rt_opt_flag_type(uint64_t entry) { (void)entry; return -1; }
int rt_opt_read(uint64_t row) { (void)row; return 0; }
int rt_opt_store(uint64_t row) { (void)row; return 0; }
int rt_opt_reset(uint64_t row) { (void)row; return 0; }
int rt_opt_msg(uint32_t id, uint64_t out) { (void)id; (void)out; return 0; }
int rt_opt_decide(uint64_t row) { (void)row; return 0; }
uint64_t rt_opt_tick(uint64_t screen) { (void)screen; return 0; }
void rt_opt_back(void) {}
int rt_opt_title_tick(uint64_t menu) { (void)menu; return 0; }
int rt_opt_pause_tick(void) { return 0; }
uint64_t rt_opt_screen(void) { return 0; }
int rt_title_event(uint32_t id) { (void)id; return 0; }
int rt_title_msg(uint64_t label, uint64_t out) { (void)label; (void)out; return 0; }
uint64_t rt_quit_dialog_begin(uint64_t owner) { (void)owner; return 0; }
void rt_quit_dialog_texts(uint64_t msg, uint64_t left, uint64_t right) { (void)msg; (void)left; (void)right; }
int rt_quit_dialog_back(void) { return 0; }

// ---- frame generation (M7: PS5_MihawkGen behind exec.c's vk_fg_generate; framegen.cpp is AMD FSR 3 on desktop)
FgVk g_fg_vk;
bool fg_dispatch(VkCommandBuffer cb, const FgConfig *c, const FgFrame *f) {
    (void)cb; (void)c; (void)f;
    static atomic_flag said = ATOMIC_FLAG_INIT;
    if (!atomic_flag_test_and_set(&said)) LOG("[fg] frame generation is not in the PS5 build before M7\n");
    return false;
}
void fg_destroy(void) {}

// ---- the performance overlay's numbers (runtime/perfmon.c; shown by the overlay menu)
void perfmon_frame(void) {}
void perfmon_generated_frame(void) {}
void perfmon_set_gpu_hint(const char *name) { (void)name; }

// ---- movies (M5: runtime/hle/movie.c, FFmpeg): no video decoder runs, so framepace.c never holds a movie's rate
volatile int g_movie_playing;
volatile int g_movie_fps = 30;
bool movie_paced(void) { return true; }
