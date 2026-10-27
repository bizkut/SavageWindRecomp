// Boundaries for subsystems deliberately not linked into the M2 title.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../rt.h"
#include <string.h>

// No renderer exists yet; kernel.c backs the CPU heap through plat_commit.
void backend_heap_set(uint64_t size) {
    fprintf(stderr, "[ps5] CPU guest heap requested: %llu bytes (renderer absent)\n", (unsigned long long)size);
}
bool nvn_timeline(void) { return false; } // no frames can be presented in M2
const char *rt_version(void) { return "0.3.1-alpha.3"; }

// A settings file can contain unrelated/unknown keys. Binding settings are
// rejected rather than silently pretending that PS5 input has been installed.
bool bind_settings_load(const char *key, const char *value) {
    (void)value;
    if (!strncmp(key, "bind_pad_", 9) || !strncmp(key, "bind_key_", 9))
        FATAL("PS5 input bindings are unavailable in M2\n");
    return false;
}

#define UNAVAILABLE(type, name, args) type name args { FATAL("PS5 M2: %s is unavailable\n", #name); }
UNAVAILABLE(void, rt_cam_reverse, (int axis, int on))
UNAVAILABLE(int, rt_cam_pitch, (float *value))
UNAVAILABLE(uint32_t, rt_cam_pitch_scale, (uint32_t value, uint32_t mode))
UNAVAILABLE(int, rt_cam_stick, (uint64_t self, uint64_t lr))
UNAVAILABLE(float, rt_arrow_gravity, (uint64_t arrow, float value))
UNAVAILABLE(float, rt_gui_speed, (float value))
UNAVAILABLE(void, rt_vfr_update, (uint64_t value))
UNAVAILABLE(int, rt_frame_wait, (void))
UNAVAILABLE(uint64_t, rt_opt_tick, (uint64_t value))
UNAVAILABLE(void, rt_opt_back, (void))
UNAVAILABLE(int, rt_opt_read, (uint64_t value))
UNAVAILABLE(int, rt_opt_msg, (uint32_t row, uint64_t out))
UNAVAILABLE(int, rt_opt_decide, (uint64_t value))
UNAVAILABLE(uint64_t, rt_quit_dialog_begin, (uint64_t value))
UNAVAILABLE(int, rt_opt_reset, (uint64_t value))
UNAVAILABLE(int, rt_opt_title_tick, (uint64_t value))
UNAVAILABLE(uint64_t, rt_opt_screen, (void))
UNAVAILABLE(int, rt_title_event, (uint32_t value))
UNAVAILABLE(int, rt_opt_store, (uint64_t value))
UNAVAILABLE(void, rt_opt_rows, (void))
UNAVAILABLE(int, rt_opt_flag_type, (uint64_t value))
UNAVAILABLE(void, rt_quit_dialog_texts, (uint64_t a, uint64_t b, uint64_t c))
UNAVAILABLE(int, rt_quit_dialog_back, (void))
UNAVAILABLE(int, rt_opt_pause_tick, (void))
UNAVAILABLE(int, rt_title_msg, (uint64_t a, uint64_t b))
UNAVAILABLE(void, rt_persp, (uint64_t p, uint32_t *a, uint32_t *b))
UNAVAILABLE(void, rt_persp_offset, (uint64_t p, uint32_t *a, uint32_t *b))
UNAVAILABLE(void, rt_frustum, (uint64_t p, float *f))
