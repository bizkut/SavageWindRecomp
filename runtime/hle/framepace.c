// Frame rate (Video > Frame rate): Off (no limit), 30, 60 (the default), 90 or 120 frames per second, and the game's
// clock the same at every one of them. Shared by the macOS, Linux and Windows builds.
//
// How BOTW 1.8.2 keeps time (main module, runtime addresses):
//   - Its frame loop (sead GameFrameworkNx, 0x700101287c) asks for a present interval of 2 vsyncs (30 fps), 3 (20 fps)
//     after three slow frames in a row. The present (nvnQueuePresentTexture: objects.c present_pace) keeps that pace;
//     the frame loop's own vsync wait (0x7000d811fc) only runs for the first frames after start-up.
//   - ksys::VFR (the instance at [0x7001d32010]) turns that interval into the game's time step: initialised with a
//     base interval of 2, so one "delta frame" is 1/30 s. Every frame VFR::updateInterval (0x7001409230) sets the
//     interval ratio (interval / 2: 1.0 at 30 fps, 1.5 at 20 fps), and delta frame = ratio x the slowest active time
//     multiplier (slow motion), delta time = delta frame / 30. About 1600 call sites read them: the time of day
//     (TimeMgr adds its step x delta frame), the Blood Moon timer, the Havok step (the physics world is handed delta
//     time every frame), stamina, animation, AI. A few read the interval itself.
//   - The menus' input timing counts frames: holding a direction on a value steps it every 6 frames on the title
//     screen (0.2 s at 30 fps, 0.1 s at 120). The paused game (pause menu, map: the state at 0x7001cb48a4 is 2, 0
//     while playing) updates VFR twice a frame.
// So the game already runs at any frame rate as long as VFR is told how long a frame really took. Savage Wind:
//   - paces the presents at the chosen rate on a steady grid (present_pace, framepace_period_ns; Off: 300 fps at
//     most, FPS_CEILING; with VSync On or Adaptive a little under the display's refresh rate), and skips the frame
//     loop's vsync wait (rt_frame_wait, tools/patches.py);
//   - tells VFR how long the frame really took (rt_vfr_update, inside updateInterval after the game set its ratio):
//     ratio = seconds x 30 (0.5 at 60 fps, 0.25 at 120): one period when the frames keep the pace, the measured time
//     averaged otherwise, changing by a few percent a frame at most, and whatever it gave more or less than the real
//     time is paid back over the next frames, so the game's time follows the real time. The interval follows the frame rate averaged over ~20 frames: 1
//     above ~43 fps (the game's own 60 fps branches, made for its debug viewer), 2 below ~35, 3 below ~23. A step is 1/20 s at most (ratio 1.5, the console's longest):
//     a hitch is paid back over the next frames, but frames slower than that for long (over 1/4 s owed) slow the
//     game down, as on the console, instead of taking steps big enough to break the physics;
//   - runs the menus (the title screen, the pause menu and the map) and the events (cutscenes, talks) at the chosen
//     frame rate too, as Cemu's FPS++ does: the menus' speed follows the frame's length (rt_gui_speed, below), and
//     movies are paced by their pictures' time (movie.c). SAVAGEWIND_MENU_FPS / SAVAGEWIND_EVENT_FPS hold them at a
//     rate (30: the console's; some events' waits count frames);
//   - steps the controllers' hold counters by the frame's length (CONTROLLER_STEP, below).
// Test and debug switches:
//   SAVAGEWIND_FPS=<n>             frame rate instead of the setting (0: no limit)
//   SAVAGEWIND_FRAMEPACE=0         the game's own pacing and clock (its 30 / 20 fps, as before)
//   SAVAGEWIND_MOVIE_FPS=<n>       frame rate while a movie plays (default: the chosen one, the pictures paced by
//                                  their time; with SAVAGEWIND_MOVIE_PACE=0 the movie's own; 0: the chosen one)
//   SAVAGEWIND_MENU_FPS=<n>        frame rate in the menus (default 0: the chosen one; 30: the console's)
//   SAVAGEWIND_EVENT_FPS=<n>       frame rate in the events (default 0: the chosen one; 30: the console's)
//   SAVAGEWIND_LOG_FRAMEPACE=<n>   every n clock updates: target, frame length, VFR ratio and interval
//   SAVAGEWIND_NO_CONTROLLER_STEP=1  the controllers count presented frames (as the game does)
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include <string.h>

#include "../input_bind.h"
#include "../game_sites.h"
#include "../hle.h"
#include "../settings.h"
#include "../services/action_prompt_restore.h"
#include "../framepace.h"
#include "../window_size.h"
#ifdef __PROSPERO__
#include "../nvn/hd_ui_generate.h"
#endif

extern volatile int g_movie_playing;  // runtime/hle/movie.c: video decoders running
extern volatile int g_movie_fps;      // runtime/hle/movie.c: the playing movie's frame rate

#define VF(T, base, off) (*(T *)(uintptr_t)((base) + (off)))
// 1.9.0 Pane constructor/destructor only. The guest's name and transform offsets come from that constructor.
typedef struct {
    uint64_t object;
    uint64_t resource;
    uint64_t context;
    uint64_t caller;
    char name[25];
    bool picture;
    bool dumped;
} PromptDebugPane;
static PromptDebugPane prompt_panes[256];
static pthread_mutex_t prompt_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
    uint64_t group, loop, root, icon0, capture, base, label[4], guide0, guide1, icon1, bar, capture_pic;
    uint64_t material, slot, original_texture, expected_texture;
    int position, initial_position;
} PromptLiveGroup;
static PromptLiveGroup prompt_live[4];
static bool prompt_live_ready;
static bool prompt_live_modified;
static bool prompt_records_changed;
static const float prompt_button_xy[4][2] = {{0, -26}, {26, 0}, {-26, 0}, {0, 26}};
static const float prompt_text_xy[4][2] = {{1, -26}, {26.25f, 0.75f}, {-25.5f, -0.25f}, {0.75f, 26}};
static const char *const prompt_icon0[] = {"Nt_KeyTexA_00", "Nt_KeyTexB_00", "Nt_KeyTexX_00", "Nt_KeyTexY_00"};
static const char *const prompt_icon1[] = {"Nt_KeyTexA_01", "Nt_KeyTexB_01", "Nt_KeyTexX_01", "Nt_KeyTexY_01"};
static const char *const prompt_capture[] = {"N_CaptureA_00", "N_CaptureB_00", "N_CaptureX_00", "N_CaptureY_00"};
static const char *const prompt_base[] = {"P_Base_00", "P_BaseB_00", "P_BaseX_00", "P_BaseY_00"};
static const char *const prompt_guide0[] = {"T_GuideA_00", "T_GuideB_00", "T_GuideX_00", "T_GuideY_00"};
static const char *const prompt_guide1[] = {"T_GuideA_01", "T_GuideB_01", "T_GuideX_01", "T_GuideY_01"};
static const char *const prompt_bar[] = {"W_BaseA_00", "W_BaseB_00", "W_BaseX_00", "W_BaseY_00"};
static const char *const prompt_labels[4][4] = {
    {NULL, "T_B_00", "T_X_00", "T_Y_00"},
    {"T_A_01", NULL, "T_X_00", "T_Y_00"},
    {"T_A_00", "T_B_00", NULL, "T_Y_00"},
    {"T_A_00", "T_B_00", "T_X_00", NULL},
};

static uint64_t prompt_parent(uint64_t object) {
    uint64_t parent;
    memcpy(&parent, (const void *)(uintptr_t)(object + 0x18), sizeof parent);
    return parent;
}
static float prompt_float(uint64_t object, unsigned offset) {
    float value;
    memcpy(&value, (const void *)(uintptr_t)(object + offset), sizeof value);
    return value;
}
static void prompt_set_float(uint64_t object, unsigned offset, float value) {
    memcpy((void *)(uintptr_t)(object + offset), &value, sizeof value);
    *(uint8_t *)(uintptr_t)(object + 0x58) |= 0x10;
}
static uint64_t prompt_find(const char *name, uint64_t parent) {
    uint64_t found = 0;
    for (size_t i = 0; i < sizeof prompt_panes / sizeof *prompt_panes; i++) {
        PromptDebugPane *p = &prompt_panes[i];
        if (!p->object || strcmp(p->name, name)) continue;
        if (strncmp((const char *)(uintptr_t)(p->object + 0xb8), name, 24)) continue;
        if (parent && prompt_parent(p->object) != parent) continue;
        if (found) return 0;
        found = p->object;
    }
    return found;
}
static bool prompt_tracked(uint64_t object) {
    for (size_t i = 0; i < sizeof prompt_panes / sizeof *prompt_panes; i++)
        if (prompt_panes[i].object == object)
            return !strncmp((const char *)(uintptr_t)(object + 0xb8), prompt_panes[i].name, 24);
    return false;
}
static bool prompt_xy_is(uint64_t object, const float xy[2]) {
    return prompt_float(object, 0x30) == xy[0] && prompt_float(object, 0x34) == xy[1];
}
static bool prompt_live_init_locked(void) {
    PromptLiveGroup groups[4] = {0};
    unsigned used = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t icon = prompt_find(prompt_icon0[i], 0);
        if (!icon) return false;
        uint64_t group = prompt_parent(icon);
        if (!group) return false;
        uint64_t loop = prompt_parent(group);
        if (!loop) return false;
        const char *group_name = i == 0 ? "N_BtnA_00" : i == 1 ? "N_BtnB_00" : "N_BtnX_00";
        if (prompt_find(group_name, loop) != group) return false;
        uint64_t root = prompt_parent(loop);
        if (!root) return false;
        PromptLiveGroup *g = &groups[i];
        g->group = group;
        g->loop = loop;
        g->root = root;
        g->icon0 = icon;
        g->capture = prompt_find(prompt_capture[i], root);
        g->base = prompt_find(prompt_base[i], loop);
        if (!g->capture || !g->base) return false;
        g->guide0 = prompt_find(prompt_guide0[i], g->capture);
        g->guide1 = prompt_find(prompt_guide1[i], group);
        g->icon1 = prompt_find(prompt_icon1[i], g->capture);
        g->bar = prompt_find(prompt_bar[i], group);
        g->capture_pic = prompt_find(i == 0 ? "P_CaptureUse00_00" : "P_CaptureUse_02", group);
        if (!g->guide0 || !g->guide1 || !g->icon1 || !g->bar || !g->capture_pic) return false;
        int position;
        for (position = 0; position < 4; position++) if (prompt_xy_is(group, prompt_button_xy[position])) break;
        if (position == 4 || (used & (1u << position))) return false;
        used |= 1u << position;
        g->position = g->initial_position = position;
        for (int j = 0; j < 4; j++) {
            if (i == j) continue;
            g->label[j] = prompt_find(prompt_labels[i][j], loop);
            if (!g->label[j]) return false;
        }
        uint64_t material, slots;
        memcpy(&material, (const void *)(uintptr_t)(g->base + 0xe0), sizeof material);
        if (!material) return false;
        memcpy(&slots, (const void *)(uintptr_t)(material + 0x18), sizeof slots);
        if (!slots) return false;
        g->material = material;
        g->slot = slots + 8;
        memcpy(&g->original_texture, (const void *)(uintptr_t)g->slot, sizeof g->original_texture);
        if (!g->original_texture) return false;
        g->expected_texture = g->original_texture;
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            if (i != j && !prompt_xy_is(groups[i].label[j], prompt_text_xy[groups[j].position])) return false;
    memcpy(prompt_live, groups, sizeof groups);
    prompt_live_ready = true;
    return true;
}

static bool prompt_live_apply_locked(const int target[4]) {
    uint64_t plate[2] = {0, 0};
    unsigned used = 0;
    for (int i = 0; i < 4; i++) {
        PromptLiveGroup *g = &prompt_live[i];
        if (!prompt_tracked(g->group) || !prompt_tracked(g->capture) || !prompt_tracked(g->base) ||
            !prompt_tracked(g->guide0) || !prompt_tracked(g->guide1) || !prompt_tracked(g->icon1) ||
            !prompt_tracked(g->bar) || !prompt_tracked(g->capture_pic)) return false;
        for (int j = 0; j < 4; j++) if (i != j && !prompt_tracked(g->label[j])) return false;
        if (!prompt_tracked(g->icon0) || prompt_parent(g->icon0) != g->group ||
            prompt_parent(g->group) != g->loop || prompt_parent(g->loop) != g->root ||
            prompt_parent(g->capture) != g->root || prompt_parent(g->base) != g->loop) return false;
        uint64_t material, slots;
        memcpy(&material, (const void *)(uintptr_t)(g->base + 0xe0), sizeof material);
        if (material != g->material) return false;
        memcpy(&slots, (const void *)(uintptr_t)(material + 0x18), sizeof slots);
        if (!slots || slots + 8 != g->slot) return false;
        if (target[i] < 0 || target[i] > 3 || (used & (1u << target[i])) ||
            !prompt_xy_is(g->group, prompt_button_xy[g->position])) return false;
        used |= 1u << target[i];
        bool left = g->position == 2;
        if (prompt_float(g->capture, 0x30) != prompt_button_xy[g->position][0] + (left ? -60 : 60) ||
            prompt_float(g->capture, 0x34) != prompt_button_xy[g->position][1] + (i == 0 ? 4 : 0) ||
            prompt_float(g->guide0, 0x30) != (left ? 37 : -39) ||
            prompt_float(g->guide1, 0x30) != (left ? -23 : 21) ||
            prompt_float(g->icon1, 0x30) != (left ? 60 : -60) ||
            prompt_float(g->bar, 0x30) != (left ? 33 : -33) ||
            prompt_float(g->bar, 0x48) != (left ? -1 : 1) ||
            prompt_float(g->capture_pic, 0x30) != (left ? -60 : 60) ||
            prompt_float(g->base, 0x44) != (g->position == 2 || g->position == 3 ? 180 : 0) ||
            prompt_float(g->base, 0x48) != (g->position == 3 ? -1 : 1)) return false;
        uint64_t current;
        memcpy(&current, (const void *)(uintptr_t)g->slot, sizeof current);
        if (current != g->expected_texture) return false;
        int family = g->position == 0 || g->position == 3;
        if (plate[family] && plate[family] != current) return false;
        plate[family] = current;
        for (int j = 0; j < 4; j++)
            if (i != j && !prompt_xy_is(g->label[j], prompt_text_xy[prompt_live[j].position])) return false;
    }
    if (!plate[0] || !plate[1] || plate[0] == plate[1]) return false;
    for (int i = 0; i < 4; i++) {
        PromptLiveGroup *g = &prompt_live[i];
        int position = target[i];
        bool left = position == 2;
        prompt_set_float(g->group, 0x30, prompt_button_xy[position][0]);
        prompt_set_float(g->group, 0x34, prompt_button_xy[position][1]);
        prompt_set_float(g->capture, 0x30, prompt_button_xy[position][0] + (left ? -60 : 60));
        prompt_set_float(g->capture, 0x34, prompt_button_xy[position][1] + (i == 0 ? 4 : 0));
        for (int j = 0; j < 4; j++) if (i != j) {
            prompt_set_float(g->label[j], 0x30, prompt_text_xy[target[j]][0]);
            prompt_set_float(g->label[j], 0x34, prompt_text_xy[target[j]][1]);
        }
        prompt_set_float(g->base, 0x44, position == 2 || position == 3 ? 180 : 0);
        prompt_set_float(g->base, 0x48, position == 3 ? -1 : 1);
        prompt_set_float(g->guide0, 0x30, left ? 37 : -39);
        prompt_set_float(g->guide1, 0x30, left ? -23 : 21);
        *(uint8_t *)(uintptr_t)(g->guide0 + 0x5b) = left ? 2 : 1;
        *(uint8_t *)(uintptr_t)(g->guide1 + 0x5b) = left ? 2 : 1;
        *(uint8_t *)(uintptr_t)(g->guide0 + 0x11e) = left ? 2 : 1;
        *(uint8_t *)(uintptr_t)(g->guide1 + 0x11e) = left ? 2 : 1;
        prompt_set_float(g->icon1, 0x30, left ? 60 : -60);
        prompt_set_float(g->capture_pic, 0x30, left ? -60 : 60);
        prompt_set_float(g->bar, 0x30, left ? 33 : -33);
        prompt_set_float(g->bar, 0x48, left ? -1 : 1);
        uint64_t texture = plate[position == 0 || position == 3];
        memcpy((void *)(uintptr_t)g->slot, &texture, sizeof texture);
        g->expected_texture = texture;
    }
    for (int i = 0; i < 4; i++) prompt_live[i].position = target[i];
    return true;
}
static void prompt_restore_locked(void) {
    if (!prompt_live_ready) return;
    if (prompt_live_modified) {
        int initial[4];
        for (int i = 0; i < 4; i++) initial[i] = prompt_live[i].initial_position;
        if (!prompt_live_apply_locked(initial)) LOG("[prompt-live] restore: pane guard failed\n");
        uint64_t *slots[4], original[4], expected[4];
        for (int i = 0; i < 4; i++) {
            slots[i] = (uint64_t *)(uintptr_t)prompt_live[i].slot;
            original[i] = prompt_live[i].original_texture;
            expected[i] = prompt_live[i].expected_texture;
        }
        if (action_prompt_restore_slots(slots, original, expected, 4) != 4)
            LOG("[prompt-live] restore: texture guard failed\n");
    }
    memset(prompt_live, 0, sizeof prompt_live);
    prompt_live_ready = false;
    prompt_live_modified = false;
}
static bool prompt_live_member(uint64_t object) {
    if (!prompt_live_ready) return false;
    for (int i = 0; i < 4; i++) {
        PromptLiveGroup *g = &prompt_live[i];
        if (object == g->group || object == g->capture || object == g->base || object == g->guide0 ||
            object == g->guide1 || object == g->icon0 || object == g->icon1 ||
            object == g->bar || object == g->capture_pic) return true;
        for (int j = 0; j < 4; j++) if (object && object == g->label[j]) return true;
    }
    return false;
}
static void prompt_live_tick(void) {
    static int skipped[4] = {-1, -1, -1, -1};
    static int previous[4] = {-1, -1, -1, -1};
    if (g_site.main_data != 0x7001b7a000ull) return;
    int target[4] = {SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH,
                     SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_WEST};
    int kind = g_settings.button_icons;
    int family = input_pad_family();
    bool pad = (family == 1 || family == 2) && kind != ICONS_KEYBOARD && kind != ICONS_NINTENDO &&
               (kind != ICONS_AUTO || input_last_device() != DEVICE_KEYBOARD);
    if (pad) for (int i = 0; i < 4; i++) target[i] = g_bind_pad[BIND_A + i];
    bool requested = memcmp(previous, target, sizeof target) != 0;
    memcpy(previous, target, sizeof target);
    pthread_mutex_lock(&prompt_lock);
    if (!prompt_live_ready) {
        bool retry = prompt_records_changed;
        prompt_records_changed = false;
        if (!retry || !prompt_live_init_locked()) {
            if (requested) LOG("[prompt-live] skipped: prompt instances not ready\n");
            pthread_mutex_unlock(&prompt_lock);
            return;
        }
    }
    bool changed = false;
    for (int i = 0; i < 4; i++) if (prompt_live[i].position != target[i]) changed = true;
    if (changed) {
        if (prompt_live_apply_locked(target)) {
            prompt_live_modified = true;
            LOG("[prompt-live] applied A=%d B=%d X=%d Y=%d\n", target[0], target[1], target[2], target[3]);
            memset(skipped, 0xff, sizeof skipped);
        } else if (memcmp(skipped, target, sizeof target)) {
            LOG("[prompt-live] skipped: invalid binding or pane/texture guard failed\n");
            memcpy(skipped, target, sizeof target);
        }
    }
    pthread_mutex_unlock(&prompt_lock);
}

static bool prompt_debug_name(const char *name) {
    static const char *const prefixes[] = {
        "N_BtnA_", "N_BtnB_", "N_BtnX_", "N_CaptureA_", "N_CaptureB_", "N_CaptureX_", "N_CaptureY_",
        "P_Base_00", "P_BaseB_", "P_BaseX_", "P_BaseY_", "T_A_0", "T_B_0", "T_X_0", "T_Y_0",
        "T_GuideA_", "T_GuideB_", "T_GuideX_", "T_GuideY_", "Nt_KeyTexA_", "Nt_KeyTexB_", "Nt_KeyTexX_",
        "Nt_KeyTexY_", "W_BaseA_", "W_BaseB_", "W_BaseX_", "W_BaseY_", "P_CaptureUse",
    };
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++)
        if (!strncmp(name, prefixes[i], strlen(prefixes[i]))) return true;
    return false;
}

void prompt_debug_entry(uint64_t addr, uint64_t object, uint64_t resource, uint64_t context, uint64_t caller) {
    if (g_site.main_data != 0x7001b7a000ull || !object) return;
    pthread_mutex_lock(&prompt_lock);
    if (addr == 0x70000268a4ull || addr == 0x70000287c4ull) {
        for (size_t i = 0; i < sizeof prompt_panes / sizeof *prompt_panes; i++) {
            if (prompt_panes[i].object != object) continue;
            if (prompt_live_member(object)) prompt_restore_locked();
            if (g_settings.action_prompt_debug)
                LOG("[prompt-dbg] destroy %s object=%llx caller=%llx\n", prompt_panes[i].name,
                    (unsigned long long)object, (unsigned long long)caller);
            memset(&prompt_panes[i], 0, sizeof prompt_panes[i]);
            prompt_records_changed = true;
        }
    } else if (resource) {
        char name[25];
        memcpy(name, (const void *)(uintptr_t)(resource + 12), 24);
        name[24] = 0;
        if (prompt_debug_name(name)) {
            for (size_t i = 0; i < sizeof prompt_panes / sizeof *prompt_panes; i++) {
                if (prompt_panes[i].object && prompt_panes[i].object != object) continue;
                prompt_panes[i] = (PromptDebugPane){object, resource, context, caller, "",
                                                   !memcmp((const void *)(uintptr_t)resource, "pic1", 4), false};
                memcpy(prompt_panes[i].name, name, sizeof name);
                prompt_records_changed = true;
                if (g_settings.action_prompt_debug)
                    LOG("[prompt-dbg] construct %s object=%llx resource=%llx context=%llx caller=%llx\n", name,
                        (unsigned long long)object, (unsigned long long)resource, (unsigned long long)context,
                        (unsigned long long)caller);
                break;
            }
        }
    }
    pthread_mutex_unlock(&prompt_lock);
}

static void prompt_debug_hex(const char *label, uint64_t address, unsigned size) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)address;
    for (unsigned off = 0; off < size; off += 16) {
        char hex[49];
        for (unsigned i = 0; i < 16; i++) snprintf(hex + i * 3, 4, "%02x ", p[off + i]);
        LOG("[prompt-dbg] %s %llx+%02x %s\n", label, (unsigned long long)address, off, hex);
    }
}

static void prompt_debug_tick(void) {
    if (!g_settings.action_prompt_debug || g_site.main_data != 0x7001b7a000ull) return;
    static uint64_t last;
    uint64_t now = plat_now_ns();
    if (last && now - last < 5000000000ull) return;
    last = now;
    pthread_mutex_lock(&prompt_lock);
    for (size_t i = 0; i < sizeof prompt_panes / sizeof *prompt_panes; i++) {
        PromptDebugPane *p = &prompt_panes[i];
        if (!p->object || (p->dumped && strcmp(p->name, "N_BtnA_00") &&
                           strncmp(p->name, "P_Base", 6))) continue;
        const uint8_t *obj = (const uint8_t *)(uintptr_t)p->object;
        if (strncmp((const char *)obj + 0xb8, p->name, 24)) continue;
        float transform[8];
        memcpy(transform, obj + 0x30, sizeof transform);
        uint64_t links[5];
        memcpy(links, obj + 8, sizeof links);
        LOG("[prompt-dbg] live %s object=%llx resource=%llx context=%llx caller=%llx "
            "translate=%g,%g,%g rotate=%g,%g,%g scale=%g,%g flags=%02x,%02x,%02x,%02x "
            "links=%llx,%llx,%llx,%llx,%llx\n",
            p->name, (unsigned long long)p->object, (unsigned long long)p->resource,
            (unsigned long long)p->context, (unsigned long long)p->caller,
            transform[0], transform[1], transform[2], transform[3], transform[4], transform[5],
            transform[6], transform[7], obj[0x58], obj[0x59], obj[0x5a], obj[0x5b],
            (unsigned long long)links[0], (unsigned long long)links[1], (unsigned long long)links[2],
            (unsigned long long)links[3], (unsigned long long)links[4]);
        if (p->picture && (!strcmp(p->name, "P_Base_00") || !strcmp(p->name, "P_BaseB_00") ||
                           !strcmp(p->name, "P_BaseX_00") || !strcmp(p->name, "P_BaseY_00"))) {
            uint64_t material, reference = 0, slots = 0, texture = 0;
            memcpy(&material, obj + 0xe0, sizeof material);
            if (material) {
                memcpy(&reference, (const void *)(uintptr_t)(material + 0x28), sizeof reference);
                memcpy(&slots, (const void *)(uintptr_t)(material + 0x18), sizeof slots);
                if (slots) memcpy(&texture, (const void *)(uintptr_t)(slots + 8), sizeof texture);
            }
            LOG("[prompt-dbg] picture=%llx material=%llx material+28=%llx slots=%llx texture=%llx\n",
                (unsigned long long)p->object, (unsigned long long)material, (unsigned long long)reference,
                (unsigned long long)slots, (unsigned long long)texture);
            if (!p->dumped) {
                prompt_debug_hex("picture", p->object, 0x100);
                // This SDK allocates exactly 0x50 bytes for a material; reading more would overrun it.
                if (material) prompt_debug_hex("material", material, 0x50);
            }
        }
        p->dumped = true;
    }
    pthread_mutex_unlock(&prompt_lock);
}
#define PAUSE_STATE g_site.pause_state  // (u32: 0 playing, 2 in the pause menu or the map)

static int env_int(const char *name, int def) {
    const char *e = getenv(name);
    return e && *e ? atoi(e) : def;
}

bool framepace_on(void) {
    static int on = -1;
    if (on < 0) on = env_int("SAVAGEWIND_FRAMEPACE", 1) != 0;
    return on;
}

// ------------------------------------------------------------------ the frame rate
// The menus: the game is paused (pause menu, map), or the title screen's menu is up (screen 1 in the UI screen
// manager [screen_mgr]: +0x28 count, +0x30 screens by id).
static bool in_menu(void) {
    if (*(uint32_t *)(uintptr_t)PAUSE_STATE) return true;
    uint64_t mgr = *(uint64_t *)(uintptr_t)g_site.screen_mgr;
    return mgr && VF(uint32_t, mgr, 0x28) > 1 && VF(uint64_t, mgr, 0x30) &&
           *(uint64_t *)(uintptr_t)(VF(uint64_t, mgr, 0x30) + 8);
}

// An event is running: a cutscene, a talk, an item fanfare (ksys::evt::Manager [event_mgr]: its active event at
// +0x1d2b8, as hasActiveEvent, 1.8.2 0x7000ffa208, reads it; the time of day stands still meanwhile). Some of their
// waits count frames: the new game's opening gave Link control 2 s early at 60 fps, 3 s at 120.
static bool in_event(void) {
    uint64_t evt = *(uint64_t *)(uintptr_t)g_site.event_mgr;
    return evt && *(uint64_t *)(uintptr_t)(evt + 0x1d2b8);
}

bool framepace_in_event(void) { return in_event(); }

// The game's UI screen `id` exists (the screen manager's array; ids as runtime/ui/game_options.mm uses them)
static bool screen_up(uint32_t id) {
    uint64_t mgr = *(uint64_t *)(uintptr_t)g_site.screen_mgr;
    if (!mgr || VF(uint32_t, mgr, 0x28) <= id) return false;
    uint64_t arr = VF(uint64_t, mgr, 0x30);
    return arr && *(uint64_t *)(uintptr_t)(arr + 8 * id);
}

// (screens: 1 the start screen, 0x38 its load and confirmation menus)
bool framepace_title_ui(void) { return screen_up(1) || screen_up(0x38); }

// (screen 0x55: the loading screen with its tip, up from a load's start until its fade into the game is over)
bool framepace_loading_ui(void) { return screen_up(0x55); }

// (pause state 2: the pause menu's pages and the map; 4 is the quick menus over the game)
bool framepace_menu_ui(void) { return *(uint32_t *)(uintptr_t)PAUSE_STATE == 2; }
bool framepace_paused(void) { return in_menu(); }

int framepace_target(void) {
    static int forced = -2, movie = -2, menu = -2, event = -2;
    if (forced == -2) forced = env_int("SAVAGEWIND_FPS", -1);
    extern bool movie_paced(void);
    if (movie == -2) movie = env_int("SAVAGEWIND_MOVIE_FPS", movie_paced() ? 0 : -1);  // (-1: the movie's own frame rate)
    if (menu == -2) menu = env_int("SAVAGEWIND_MENU_FPS", 0);
    if (event == -2) event = env_int("SAVAGEWIND_EVENT_FPS", 0);
    int fps = forced >= 0 ? forced : g_settings.fps_limit;
    if (fps < 0) fps = FPS_DEFAULT;
    if (movie != 0 && __atomic_load_n(&g_movie_playing, __ATOMIC_RELAXED) > 0) {
        int cap = movie > 0 ? movie : g_movie_fps;
        if (fps == 0 || fps > cap) fps = cap;
    }
    if (menu > 0 && (fps == 0 || fps > menu) && in_menu()) fps = menu;
    if (event > 0 && (fps == 0 || fps > event) && in_event()) fps = event;
    return fps;
}

// No limit still stops at 300 fps: shorter steps lose precision in the game's float clocks (the time of day, 0..360,
// takes 1/30 of a minute times the step each frame: at 300 fps that is 1% of the value's rounding unit per frame).
#define FPS_CEILING 300

// With VSync On or Adaptive (FIFO), a frame rate at or over the display's refresh rate waits for the display: up to a
// refresh of delay, and with G-SYNC / FreeSync the display leaves its variable rate at the top of its range. So the
// frame rate stays under it there, at refresh - refresh^2 / 3600 (NVIDIA Reflex's cap: 59 fps at 60 Hz, 138 at 144,
// 224 at 240). Fast (MAILBOX) and Off (IMMEDIATE) never wait for the display: no cap.
static double vsync_cap(void) {
    if (g_settings.vsync != VSYNC_ON && g_settings.vsync != VSYNC_ADAPTIVE) return 0;
    double hz = winsize_refresh_rate();
    return hz >= 30 ? hz - hz * hz / 3600 : 0;
}

// A frame rate within 1% of the refresh rate divided by a whole number takes exactly that (60 on a 239.77 Hz display:
// 59.94, a picture every 4 refreshes; 30: 29.97). Otherwise the frames drift against the refreshes, and on a display
// without G-SYNC / FreeSync one in a few hundred waits a refresh less or more (a judder every few seconds). With
// G-SYNC / FreeSync it changes nothing that shows. Not with VSync Off, which doesn't wait for refreshes.
static double on_refresh(double fps) {
    double hz = g_settings.vsync == VSYNC_OFF ? 0 : winsize_refresh_rate();
    if (hz < 30 || fps > hz) return fps;
    double n = round(hz / fps);
    return n >= 1 && fabs(hz / n - fps) < fps * 0.01 ? hz / n : fps;
}

// With frame generation (Video > Frame generation, vulkan/framegen.h) a frame is made between each two the game draws,
// presented first, and the game's half a frame later (vulkan/device.c present_wait_turn). What the frame-rate limit
// counts is the backend's:
//   - desktop's FSR 3: the presented frames, so the game draws at half of it (60 with 120 chosen);
//   - the PS5's MihawkGen: the game's frames, each presented twice (docs/PS5-PORT-PLAN.md M7: 60 gives ~120 presents,
//     one every ~8.4 ms at 119.88 Hz under VRR), at most 0.5 % under half the refresh rate: 59.64 fps and 119.28
//     presents a second at 119.88 Hz (59.94 with frame generation off; the menus' frames, not generated, keep that).
//     Presents at exactly the refresh rate give FIFO no way to drain what
//     queues up (each late frame leaves one more present waiting, until no window image is free: bench4-fg1.klog, the
//     render thread waited 5.45 ms a frame to acquire one); a little under it, VRR shows each present as it comes and
//     the queue empties. The VSync cap below (VRR's margin for frames at the refresh rate, 3.3 % at 120 Hz) is not
//     applied: it would hold the game at 57.9 fps.
volatile int g_fg_active;       // (vulkan/device.c: the last game frame was presented after a generated one)
volatile int g_fg_counts_game;  // (vulkan/device.c: the backend's limit counts the game's frames: MihawkGen)

uint64_t framepace_period_ns(void) {
    int target = framepace_target();
    double fps = target > 0 ? target : FPS_CEILING, cap = vsync_cap();
    if (g_fg_active && g_fg_counts_game) {
        double hz = winsize_refresh_rate(), most = hz / 2 * 0.995;
        if (hz >= 30 && fps > most) return (uint64_t)(1e9 / most + 0.5);  // (not snapped to the refresh: on_refresh)
        return (uint64_t)(1e9 / on_refresh(fps) + 0.5);
    }
    if (cap > 0 && fps > cap) fps = cap;
    return (uint64_t)((g_fg_active ? 2e9 : 1e9) / on_refresh(fps) + 0.5);
}

uint64_t framepace_fg_period_ns(void) {
    return g_fg_active && g_fg_counts_game && framepace_on() ? framepace_period_ns() : 0;
}

// The frame loop's vsync wait: the present keeps the pace instead (1: skipped; 0: the game's own wait).
int rt_frame_wait(void) { return framepace_on(); }

// ------------------------------------------------------------------ presents
// The controllers (sead::ControllerBase::updateDerivativeParams_, 1.8.2 0x7000d9d8a0) count how long each button has
// been held, in frames: a held button's repeat starts after its delay and comes every pulse frames, and the game times
// some holds with the count. The count goes up by a step the game keeps at 1 (a float, controller_step: 1.8.2
// 0x7001c4ba40, read there only): the runtime sets it to the frame's length in console frames, so holds last as long
// as on the console.
#define CONTROLLER_STEP g_site.controller_step
static float g_step = 1;         // the last frame's length between presents, in console frames
static int g_grid_streak;        // presents in a row that kept the pace

void framepace_presented(uint64_t t, bool on_grid) {
    if (!framepace_on()) return;
    static uint64_t prev;
    static double smooth;
    double s = prev && t > prev ? (t - prev) / 1e9 : 1.0 / 30;
    prev = t;
    if (s > 0.25) s = 0.25;
    smooth = smooth > 0 ? smooth + (s - smooth) * 0.25 : s;
    float step = (float)((on_grid ? s : smooth) * 30);
    g_step = step < 0.1f ? 0.1f : step > 1.5f ? 1.5f : step;
    g_grid_streak = on_grid ? g_grid_streak + 1 : 0;
    static int fixed = -1;
    if (fixed < 0) fixed = env_int("SAVAGEWIND_NO_CONTROLLER_STEP", 0);
    if (!fixed) *(float *)(uintptr_t)CONTROLLER_STEP = g_step;
}

// The menus' speed: every frame the game sets a GUI speed to its integer interval x 0.5: 1 at 30 fps, 0.5 at 60, and
// never lower, so the menus' cursor and animations ran faster above 60 fps (and at 60 when the interval follows the
// frame rate). It becomes the frame's length in console frames, as Cemu's FPS++ does on the Wii U.
// SAVAGEWIND_NO_GUI_SPEED=1: the game's own.
float rt_gui_speed(float game) {
    static int off = -1;
    if (off < 0) off = env_int("SAVAGEWIND_NO_GUI_SPEED", 0);
    return off || !framepace_on() ? game : g_step;
}

// ------------------------------------------------------------------ the game's clock (ksys::VFR)
// VFR fields (1.8.2; the layout of zeldaret's ksys::VFR): +0x28 interval changed, +0x29 interval override on,
// +0x30 new interval ratio, +0x34 interval, +0x3c interval ratio (both read through per-core pointers).
#define VFR_CHANGED 0x28
#define VFR_OVERRIDE 0x29
#define VFR_NEW_RATIO 0x30
#define VFR_INTERVAL 0x34
#define VFR_RATIO 0x3c

static void clock_probe(uint64_t vfr);
static void log_screens(void);

// The frame's length in console frames (VFR's interval ratio as set below: 1 at 30 fps, 0.5 at 60), for game code
// patched to scale a per-update step it doesn't scale itself
static volatile float g_frame_ratio = 1;

// An arrow's gravity in its fall, added once per update (tools/patches.py _arrow_gravity, #144): per console frame.
// SAVAGEWIND_LOG_ARROW=1 logs each falling arrow's update (time, velocity before it).
float rt_arrow_gravity(uint64_t arrow, float g) {
    static int log = -1;
    if (log < 0) log = env_int("SAVAGEWIND_LOG_ARROW", 0);
    if (log > 0)
        LOG("[arrow] %.3f s %#llx ratio %.3f gravity %.2f velocity %.2f %.2f %.2f\n", plat_now_ns() / 1e9,
            (unsigned long long)arrow, g_frame_ratio, g, VF(float, arrow, 0x110), VF(float, arrow, 0x114),
            VF(float, arrow, 0x118));
    return g * g_frame_ratio;
}
static uint64_t game_frames;  // rt_vfr_update calls: the game's frames (framepace_game_frame)
uint64_t framepace_game_frame(void) { return __atomic_load_n(&game_frames, __ATOMIC_RELAXED); }
// The game's frames that were presented: one step per presented frame, where game frames step twice while paused
// (output_scale.c's jitter sequence: each presented frame gets the next phase)
static uint64_t shown_frames;
uint64_t framepace_shown_frame(void) { return __atomic_load_n(&shown_frames, __ATOMIC_RELAXED); }

void rt_vfr_update(uint64_t vfr) {
    prompt_live_tick();
    prompt_debug_tick();
    __atomic_fetch_add(&game_frames, 1, __ATOMIC_RELAXED);
    static int ui_state = -1;
    int ui = framepace_loading_ui() ? 2 : framepace_title_ui() ? 1 : 0;
#ifdef __PROSPERO__
    if (ui != ui_state) hd_ui_generate_set_loading(ui == 2);
    static bool startup_title_seen;
    if (!startup_title_seen && framepace_title_ui()) {
        extern void ps5_startup_mark(const char *step);
        startup_title_seen = true;
        ps5_startup_mark("title screen first seen");
    }
#endif
    if (ui != ui_state) {
        static const char *const names[] = {"play", "title", "loading"};
        LOG("[ui] screen: %s\n", names[ui]);
        ui_state = ui;
    }
    {
        extern uint64_t renderer_present_count(void);
        static uint64_t presented = ~0ull;
        uint64_t n = renderer_present_count();
        if (n != presented) presented = n, __atomic_fetch_add(&shown_frames, 1, __ATOMIC_RELAXED);
    }
    extern bool nvn_timeline(void);
    if (nvn_timeline()) LOG("[tl] %.3f %x vfr\n", plat_now_ns() / 1e6, kernel_current_thread_handle());
    extern uint64_t g_fs_vfr;  // (SAVAGEWIND_FRAME_LOG=2, nvn/objects.c)
    g_fs_vfr = plat_now_ns();
    clock_probe(vfr);
    log_screens();
    extern void input_cam_frame(float ratio);  // runtime/hle/input.c: the mouse's motion in the frame, for the cameras
    if (!framepace_on()) {
        g_frame_ratio = VF(float, vfr, VFR_RATIO);
        input_cam_frame(VF(float, vfr, VFR_RATIO));
        return;
    }
    static uint64_t last_t;
    static double real, given;  // seconds of real time and of game time handed out (only their gap matters)
    static double smooth;       // seconds between updates, averaged over about 20
    static double last_step;    // the step handed out last, seconds (0: none yet)
    static int last_target = -1;
    static double slow = 1;     // the step in console frames, averaged over about 20 (the interval follows it)
    static uint32_t last_iv;
    static int log_every = -1;
    static uint64_t nframe;
    if (log_every < 0) log_every = env_int("SAVAGEWIND_LOG_FRAMEPACE", 0);
    uint64_t now = plat_now_ns();
    double s = last_t && now > last_t ? (now - last_t) / 1e9 : 1.0 / 30;
    last_t = now;
    float ratio;
    bool paused = *(uint32_t *)(uintptr_t)PAUSE_STATE != 0;
    if (paused) {
        // paused (it updates twice a frame): the frame's length between presents, nothing owed
        ratio = g_step;
        real = given = 0;
        last_target = -1;
    } else {
        if (s > 0.25) s = 0.25;  // (a load or a pause in a debugger: not owed)
        double period = framepace_period_ns() / 1e9;
        int target = framepace_target();
        if (target != last_target) smooth = period, last_step = 0;  // (a new pace: start from it)
        last_target = target;
        smooth += (s - smooth) * 0.05;
        real += s;
        // The step changes slowly: the game reads some deltas a frame late, and a step that jumps between frames
        // moves the camera and Link apart (Link seemed to step back in place, seen from the side). One period once
        // the presents have kept the pace for half a second, else the average of ~20 frames; plus a fiftieth of what
        // is owed; and at most 3% away from the last step.
        double step = (g_grid_streak >= 30 ? period : smooth) + (real - given) * 0.02;
        if (last_step > 0 && step > last_step * 1.03) step = last_step * 1.03;
        if (last_step > 0 && step < last_step * 0.97) step = last_step * 0.97;
        if (step < 1.0 / FPS_CEILING) step = 1.0 / FPS_CEILING;
        if (step > 0.05) step = 0.05;
        last_step = step;
        ratio = (float)(step * 30);
    }
    bool held = VF(uint8_t, vfr, VFR_OVERRIDE) && ratio > 1;  // (stage loading: the game asks for single steps)
    if (held) ratio = 1;
    if (!paused) {
        given += ratio / 30.0;
        if (held || fabs(real - given) > 0.25) real = given;  // (nothing owed for a deliberate slow down)
    }
    // The interval switches some of the game's code between its 60 fps and 30 fps ways: only on a lasting change of
    // the frame rate (above ~43 fps: 1; below ~35: 2; below ~23: 3), never back and forth from one frame to the next.
    slow += (ratio - slow) * 0.05;
    static uint32_t iv = 2;
    if (iv == 1 && slow > 0.85) iv = 2;
    else if (iv == 2 && slow < 0.7) iv = 1;
    else if (iv == 2 && slow > 1.3) iv = 3;
    else if (iv == 3 && slow < 1.15) iv = 2;
    VF(float, vfr, VFR_RATIO) = ratio;
    VF(float, vfr, VFR_NEW_RATIO) = ratio;
    if (!paused) g_frame_ratio = ratio;
    VF(uint32_t, vfr, VFR_INTERVAL) = iv;
    VF(uint8_t, vfr, VFR_CHANGED) = last_iv && iv != last_iv;
    last_iv = iv;
    input_cam_frame(ratio);
    if (log_every > 0 && nframe++ % (uint64_t)log_every == 0) {
        uint64_t pinfo = *(uint64_t *)(uintptr_t)g_site.player_info;  // (Link's position: PlayerInfo +0x88)
        LOG("[framepace] target %d fps, %.2f ms since the last update (avg %.2f), ratio %.3f, interval %u, owed "
            "%.2f ms, Link %.3f %.3f%s\n", framepace_target(), s * 1e3, smooth * 1e3, ratio, iv, (real - given) * 1e3,
            pinfo ? VF(float, pinfo, 0x88) : 0.0f, pinfo ? VF(float, pinfo, 0x90) : 0.0f,
            g_movie_playing > 0 ? ", movie" : "");
    }
}

// ------------------------------------------------------------------ test scripts, the clock probe, the screens
// SAVAGEWIND_SCRIPT_CLOCK=1: SAVAGEWIND_INPUT and SAVAGEWIND_SHOT_AT count 30ths of a second (the console's frames)
// instead of presented frames, so one script drives the game the same way at any frame rate: each presented frame
// adds its length x 30, at most 1.5 (a load's long frames count like the console's).
uint64_t framepace_script_frame(uint64_t presented) {
    static int on = -1;
    static double t;
    static uint64_t last;
    if (on < 0) on = env_int("SAVAGEWIND_SCRIPT_CLOCK", 0) != 0;
    if (!on) return presented;
    uint64_t now = plat_now_ns();
    double units = last ? (now - last) / 1e9 * 30 : 1;
    last = now;
    t += units > 1.5 ? 1.5 : units;
    {   // (debug) SAVAGEWIND_DUMP_DATA_AT=<frame>,...: main's .data and .bss -> build/data_<frame>.bin
        static const char *next;
        static int init;
        if (!init) init = 1, next = getenv("SAVAGEWIND_DUMP_DATA_AT");
        if (next && *next && (uint64_t)t >= strtoull(next, NULL, 10)) {
            char path[64];
            snprintf(path, sizeof path, "build/data_%llu.bin", (unsigned long long)t);
            FILE *f = fopen(path, "wb");
            if (f) fwrite((void *)(uintptr_t)g_site.main_data, 1, g_site.main_end - g_site.main_data, f), fclose(f);
            next = strchr(next, ',');
            if (next) next++;
        }
    }
    return (uint64_t)t;
}

// SAVAGEWIND_CLOCK_PROBE=<ms>: every <ms> of real time, a "[clock]" line with what the frame rate must not change:
// seconds since the probe started, frames, the game's own time (its delta frames summed, 1/30 s each, slow motion
// included), the time of day (TimeMgr, 0..360 = 24 h: one in-game minute is 0.25), Link's position and stamina
// (1000 a wheel): ksys::world::Manager [world_mgr] (+0x5c0 count, +0x5c8 its managers; [0] = TimeMgr, the time at
// +0xb8); uking::act::PlayerInfo [player_info] (+0x88 Link's position, +0xa4 the stamina).
static void clock_probe(uint64_t vfr) {
    static int every = -1;
    static uint64_t t0, next, frames;
    static double game;
    if (every < 0) every = env_int("SAVAGEWIND_CLOCK_PROBE", 0);
    if (every <= 0) return;
    frames++;
    game += VF(float, vfr, 0x98);  // (core 0's delta frame of the frame that just ended)
    uint64_t now = plat_now_ns();
    if (!t0) t0 = now, next = now;
    if (now < next) return;
    next += (uint64_t)every * 1000000ull;
    if (next < now) next = now + (uint64_t)every * 1000000ull;
    float tod = -1, pos[3] = {0, 0, 0}, stamina = -1;
    uint64_t wm = *(uint64_t *)(uintptr_t)g_site.world_mgr;
    if (wm && VF(uint32_t, wm, 0x5c0)) {
        uint64_t mgrs = VF(uint64_t, wm, 0x5c8), tm = mgrs ? *(uint64_t *)(uintptr_t)mgrs : 0;
        if (tm) tod = VF(float, tm, 0xb8);
    }
    uint64_t pinfo = *(uint64_t *)(uintptr_t)g_site.player_info;
    if (pinfo) {
        for (int i = 0; i < 3; i++) pos[i] = VF(float, pinfo, 0x88 + 4 * i);
        stamina = VF(float, pinfo, 0xa4);
    }
    LOG("[clock] t %.3f frames %llu game %.2f tod %.4f pos %.3f %.3f %.3f stamina %.1f fps %d at %.3f\n",
        (now - t0) / 1e9, (unsigned long long)frames, game, tod, pos[0], pos[1], pos[2], stamina, framepace_target(),
        now / 1e9);
}

// SAVAGEWIND_LOG_SCREENS=1: the pause state and the game's UI screens that exist (their ids), each time they change
static void log_screens(void) {
    static int on = -1;
    static char last[1024];
    if (on < 0) on = env_int("SAVAGEWIND_LOG_SCREENS", 0);
    if (!on) return;
    uint64_t mgr = *(uint64_t *)(uintptr_t)g_site.screen_mgr;
    if (!mgr) return;
    uint32_t n = VF(uint32_t, mgr, 0x28);
    uint64_t arr = VF(uint64_t, mgr, 0x30);
    char cur[1024];
    int k = snprintf(cur, sizeof cur, " (pause state %u):", *(uint32_t *)(uintptr_t)PAUSE_STATE);
    for (uint32_t i = 0; i < n && i < 256 && arr && k < (int)sizeof cur - 8; i++)
        if (*(uint64_t *)(uintptr_t)(arr + 8 * i)) k += snprintf(cur + k, sizeof cur - k, " %x", i);
    if (strcmp(cur, last)) {
        LOG("[screens]%s\n", cur);
        strcpy(last, cur);
    }
}
