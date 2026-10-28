// Mouse wheel zoom: one notch, one pulse of the Switch input the open screen zooms with (runtime/wheel_zoom.h).
//
// What zooms where (1.9.0's code; the screens by their layouts' names, the ScreenMgr's table at 0x7101d4e3e8):
// - The scope (and the camera rune): screen 4, AppCamera_00. Its update (FUN_7100de8ebc) zooms while the d-pad's Up
//   or Down is held (its controller's held bits 16 / 17, sead's Up / Down, which the Npad controller's update makes
//   from nn::hid's Up / Down alone, 0x71010fe678), speeding up the longer it is held, and keeps the zoom in a game data
//   float. A pulse holds Up (in) or Down (out) for SCOPE_ON frames; notches that follow keep it held.
// - The map: screen 0x2a, AppMap_00. Its input (FUN_7100dfa50c) steps one zoom level in on the right stick's
//   RightStickUp trigger, out on RightStickDown (bits 24 / 25: sead derives them from the stick's tilt), and nothing
//   while it stays tilted. A pulse tilts the right stick fully up or down for MAP_ON frames, then centres it for
//   MAP_OFF frames, so the next one triggers again.
// A screen counts while its open state byte (game site screen_state, 1.9.0 +0xfe, as the game's own isOpen
// FUN_7100f6e08c reads it) says open: 2, after its opening and before its closing animation. A screen can be there
// while it is closed (the screen open request, FUN_7100e6a590, checks whether the map's screen is closed), so being
// there is not enough.
#include <stdatomic.h>

#include "../input_bind.h"
#include "../rt.h"
#include "../wheel_zoom.h"

#define SCREEN_CAMERA 4    // AppCamera_00: the scope, the camera rune
#define SCREEN_MAP 0x2a    // AppMap_00
#define SCREEN_OPEN 2      // the open state byte: 0 closed, 1 opening, 2 open, 3 closing
#define SCOPE_ON 4         // frames of d-pad per notch (from rest: about a tenth of the scope's range)
#define MAP_ON 2           // frames of right stick per notch, then
#define MAP_OFF 2          // frames centred
#define MAX_PENDING 8      // notches waiting at most (a fast spin doesn't keep zooming long after)

static const char *const k_names[] = {"off", "scope", "map"};

static _Atomic int published = WZ_NONE;  // the context at the last controller read: the main thread's gate
static _Atomic int pending;              // notches waiting: > 0 in, < 0 out
static float part;                       // main thread: the part of a notch a fine wheel or touchpad gave so far

static uint64_t screen(uint64_t mgr, uint32_t id) {  // (ScreenMgr: +0x28 count, +0x30 screens by id)
    uint64_t arr = *(uint64_t *)(uintptr_t)(mgr + 0x30);
    return id < *(uint32_t *)(uintptr_t)(mgr + 0x28) && arr ? *(uint64_t *)(uintptr_t)(arr + 8 * (uint64_t)id) : 0;
}

static bool screen_open(uint64_t s, uint64_t state_field) {
    return s && *(uint8_t *)(uintptr_t)(s + state_field) == SCREEN_OPEN;
}

int wheel_zoom_context(uint64_t screen_mgr, uint64_t state_field) {
    uint64_t mgr = screen_mgr && state_field ? *(uint64_t *)(uintptr_t)screen_mgr : 0;
    if (!mgr) return WZ_NONE;
    if (screen_open(screen(mgr, SCREEN_MAP), state_field)) return WZ_MAP;
    if (screen_open(screen(mgr, SCREEN_CAMERA), state_field)) return WZ_SCOPE;
    return WZ_NONE;
}

void wheel_zoom_wheel(float y) {
    if (atomic_load(&published) == WZ_NONE) {
        part = 0;
        return;
    }
    part += y;
    int n = (int)part;  // (toward 0: a part left over waits for the rest of its notch)
    if (!n) return;
    part -= (float)n;
    int p = atomic_load(&pending), q;
    do {
        q = p + n;
        q = q > MAX_PENDING ? MAX_PENDING : q < -MAX_PENDING ? -MAX_PENDING : q;
    } while (!atomic_compare_exchange_weak(&pending, &p, q));
}

// (game thread: the controller reads come from the game's controller update, one thread)
static struct {
    int ctx, dir, phase;  // phase: 0 idle, 1 pulse, 2 centred after a map pulse
    uint64_t until;       // the frame the phase ends at
} g;

WheelZoomPulse wheel_zoom_read(int ctx, uint64_t frame) {
    WheelZoomPulse out = {0};
    atomic_store(&published, ctx);
    if (ctx != g.ctx) {  // opened, closed or another screen: what waited was for the one before
        LOG("[input] wheel zoom: %s\n", k_names[ctx]);
        g.ctx = ctx, g.phase = 0;
        atomic_store(&pending, 0);
    }
    if (ctx == WZ_NONE) return out;
    if (g.phase == 1 && frame >= g.until) g.phase = ctx == WZ_MAP ? 2 : 0, g.until = frame + MAP_OFF;
    if (g.phase == 2 && frame >= g.until) g.phase = 0;
    if (g.phase == 0) {
        int p = atomic_load(&pending);
        while (p && !atomic_compare_exchange_weak(&pending, &p, p > 0 ? p - 1 : p + 1)) {}
        if (p) {
            g.dir = p > 0 ? 1 : -1, g.phase = 1, g.until = frame + (ctx == WZ_MAP ? MAP_ON : SCOPE_ON);
            LOG("[input] wheel zoom: %s %s (%s)\n", k_names[ctx], g.dir > 0 ? "in" : "out",
                ctx == WZ_MAP ? (g.dir > 0 ? "right stick up" : "right stick down") : (g.dir > 0 ? "d-pad up" : "d-pad down"));
        }
    }
    if (g.phase == 1 && ctx == WZ_SCOPE) out.buttons = 1ull << (g.dir > 0 ? BIND_UP : BIND_DOWN);
    if (g.phase == 1 && ctx == WZ_MAP) out.stick = true, out.ry = g.dir * 32767;
    if (g.phase == 2) out.stick = true;  // (centred: the stick has to come back for the next trigger)
    return out;
}
