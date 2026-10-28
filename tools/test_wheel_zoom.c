// runtime/hle/wheel_zoom.c with no game: what counts as the scope or the map being open (a made-up ScreenMgr and
// screens), and the pulses a wheel's notches turn into, frame by frame. make test-wheel-zoom
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/hle/wheel_zoom.c"

#define STATE 0xfe  // (1.9.0's screen_state)
#define UP (1ull << BIND_UP)
#define DOWN (1ull << BIND_DOWN)

// ---- a ScreenMgr (+0x28 count, +0x30 screens by id) and two screens
static uint8_t mgr[0x40], camera[0x100], map[0x100];
static uint64_t screens[0x60];
static uint64_t mgr_ptr;  // (what the screen_mgr site points at)

static void screens_reset(void) {
    memset(mgr, 0, sizeof mgr), memset(camera, 0, sizeof camera), memset(map, 0, sizeof map);
    memset(screens, 0, sizeof screens);
    *(uint32_t *)(mgr + 0x28) = 0x60;
    *(uint64_t *)(mgr + 0x30) = (uint64_t)(uintptr_t)screens;
    mgr_ptr = (uint64_t)(uintptr_t)mgr;
    screens[SCREEN_CAMERA] = (uint64_t)(uintptr_t)camera;
    screens[SCREEN_MAP] = (uint64_t)(uintptr_t)map;
}

static int ctx(void) { return wheel_zoom_context((uint64_t)(uintptr_t)&mgr_ptr, STATE); }

static void test_context(void) {
    screens_reset();
    assert(ctx() == WZ_NONE);  // both there, both closed
    for (int s = 0; s < 4; s++) {
        camera[STATE] = (uint8_t)s;
        assert(ctx() == (s == 2 ? WZ_SCOPE : WZ_NONE));  // opening (1) and closing (3) don't count
    }
    camera[STATE] = 0;
    for (int s = 0; s < 4; s++) {
        map[STATE] = (uint8_t)s;
        assert(ctx() == (s == 2 ? WZ_MAP : WZ_NONE));
    }
    camera[STATE] = 2, map[STATE] = 2;
    assert(ctx() == WZ_MAP);  // (both: the map, over the play)
    map[STATE] = 0;
    assert(ctx() == WZ_SCOPE);
    map[STATE + 1] = 2, camera[STATE - 1] = 2, camera[STATE] = 0;  // (only the state byte counts)
    assert(ctx() == WZ_NONE);
    camera[STATE] = 2;
    assert(wheel_zoom_context((uint64_t)(uintptr_t)&mgr_ptr, 0) == WZ_NONE);  // the game code has no screen_state
    assert(wheel_zoom_context(0, STATE) == WZ_NONE);                          // ... or no screen_mgr
    mgr_ptr = 0;
    assert(ctx() == WZ_NONE);  // no ScreenMgr yet (boot)
    mgr_ptr = (uint64_t)(uintptr_t)mgr;
    screens[SCREEN_CAMERA] = 0;
    assert(ctx() == WZ_NONE);  // no camera screen made
    screens[SCREEN_CAMERA] = (uint64_t)(uintptr_t)camera;
    *(uint32_t *)(mgr + 0x28) = SCREEN_CAMERA;
    assert(ctx() == WZ_NONE);  // fewer screens than its id
    *(uint32_t *)(mgr + 0x28) = 0x60;
    *(uint64_t *)(mgr + 0x30) = 0;
    assert(ctx() == WZ_NONE);  // no screen array
    *(uint64_t *)(mgr + 0x30) = (uint64_t)(uintptr_t)screens;
    assert(ctx() == WZ_SCOPE);
}

static uint64_t frame = 1000;

// One frame of controller reads (the game polls several times a frame: each read of a frame gives the same)
static WheelZoomPulse step(int c) {
    WheelZoomPulse a = wheel_zoom_read(c, frame), b = wheel_zoom_read(c, frame);
    assert(a.buttons == b.buttons && a.stick == b.stick && a.rx == b.rx && a.ry == b.ry);
    frame++;
    return a;
}

static bool idle(WheelZoomPulse p) { return !p.buttons && !p.stick; }

static void test_off(void) {
    step(WZ_NONE);
    wheel_zoom_wheel(3);  // nothing open: the notches are dropped at once, not kept for later
    assert(atomic_load(&pending) == 0);
    wheel_zoom_wheel(0.6f), wheel_zoom_wheel(0.6f);
    assert(atomic_load(&pending) == 0 && part == 0);
    for (int i = 0; i < 4; i++) assert(idle(step(WZ_NONE)));
    for (int i = 0; i < 12; i++) assert(idle(step(WZ_SCOPE)));
    for (int i = 0; i < 12; i++) assert(idle(step(WZ_MAP)));
}

static void test_scope(void) {
    step(WZ_NONE), step(WZ_SCOPE);
    wheel_zoom_wheel(1);  // one notch away: d-pad Up for SCOPE_ON frames
    for (int i = 0; i < SCOPE_ON; i++) {
        WheelZoomPulse p = step(WZ_SCOPE);
        assert(p.buttons == UP && !p.stick);
    }
    assert(idle(step(WZ_SCOPE)));
    wheel_zoom_wheel(-1);  // toward the player: Down
    for (int i = 0; i < SCOPE_ON; i++) assert(step(WZ_SCOPE).buttons == DOWN);
    assert(idle(step(WZ_SCOPE)));
    wheel_zoom_wheel(2);  // notches in a row: one hold, so the game's zoom keeps speeding up
    for (int i = 0; i < 2 * SCOPE_ON; i++) assert(step(WZ_SCOPE).buttons == UP);
    assert(idle(step(WZ_SCOPE)));
    wheel_zoom_wheel(1);  // turned back while it holds: the next pulse goes the other way
    assert(step(WZ_SCOPE).buttons == UP);
    wheel_zoom_wheel(-1);
    for (int i = 1; i < SCOPE_ON; i++) assert(step(WZ_SCOPE).buttons == UP);
    for (int i = 0; i < SCOPE_ON; i++) assert(step(WZ_SCOPE).buttons == DOWN);
    assert(idle(step(WZ_SCOPE)));
    wheel_zoom_wheel(3), wheel_zoom_wheel(-1);  // waiting notches add up: 2 in
    for (int i = 0; i < 2 * SCOPE_ON; i++) assert(step(WZ_SCOPE).buttons == UP);
    assert(idle(step(WZ_SCOPE)));
}

static void test_map(void) {
    step(WZ_NONE), step(WZ_MAP);
    wheel_zoom_wheel(2);  // each notch: the right stick fully up, then centred, so each one triggers
    for (int n = 0; n < 2; n++) {
        for (int i = 0; i < MAP_ON; i++) {
            WheelZoomPulse p = step(WZ_MAP);
            assert(!p.buttons && p.stick && p.rx == 0 && p.ry == 32767);
        }
        for (int i = 0; i < MAP_OFF; i++) {
            WheelZoomPulse p = step(WZ_MAP);
            assert(!p.buttons && p.stick && p.rx == 0 && p.ry == 0);
        }
    }
    assert(idle(step(WZ_MAP)));  // done: the player's stick again
    wheel_zoom_wheel(-1);
    for (int i = 0; i < MAP_ON; i++) assert(step(WZ_MAP).ry == -32767);
    for (int i = 0; i < MAP_OFF; i++) assert(step(WZ_MAP).ry == 0);
    assert(idle(step(WZ_MAP)));
}

static void test_fine_wheel(void) {
    step(WZ_NONE), step(WZ_SCOPE);
    wheel_zoom_wheel(0.4f), wheel_zoom_wheel(0.4f);  // a high-resolution wheel or touchpad: parts of a notch
    assert(idle(step(WZ_SCOPE)));
    wheel_zoom_wheel(0.4f);  // 1.2: one notch, 0.2 kept
    for (int i = 0; i < SCOPE_ON; i++) assert(step(WZ_SCOPE).buttons == UP);
    assert(idle(step(WZ_SCOPE)));
    wheel_zoom_wheel(0.8f);  // 0.2 + 0.8: the next one
    for (int i = 0; i < SCOPE_ON; i++) assert(step(WZ_SCOPE).buttons == UP);
    assert(idle(step(WZ_SCOPE)));
    wheel_zoom_wheel(0.5f);
    step(WZ_NONE), step(WZ_SCOPE);  // closed meanwhile: the part is dropped with the rest
    wheel_zoom_wheel(0.0f), wheel_zoom_wheel(0.6f);
    assert(idle(step(WZ_SCOPE)));
}

static void test_cap(void) {
    step(WZ_NONE), step(WZ_SCOPE);
    wheel_zoom_wheel(30);  // a fast spin: at most MAX_PENDING notches wait
    int frames = 0;
    while (step(WZ_SCOPE).buttons == UP) frames++;
    assert(frames == MAX_PENDING * SCOPE_ON);
    wheel_zoom_wheel(-30);
    for (frames = 0; step(WZ_SCOPE).buttons == DOWN;) frames++;
    assert(frames == MAX_PENDING * SCOPE_ON);
}

static void test_close(void) {
    step(WZ_NONE), step(WZ_SCOPE);
    wheel_zoom_wheel(3);
    assert(step(WZ_SCOPE).buttons == UP);
    assert(idle(step(WZ_NONE)));  // the scope closes mid-pulse: no Up past it (Up opens the rune menu in play)
    for (int i = 0; i < 16; i++) assert(idle(step(WZ_SCOPE)));  // what waited is gone
    wheel_zoom_wheel(3);
    assert(step(WZ_SCOPE).buttons == UP);
    assert(idle(step(WZ_MAP)));  // the scope's notches don't zoom the map that replaced it
    for (int i = 0; i < 16; i++) assert(idle(step(WZ_MAP)));
    wheel_zoom_wheel(1);
    assert(step(WZ_MAP).ry == 32767);
    WheelZoomPulse p = step(WZ_SCOPE);  // map to scope mid-pulse: no stick and no d-pad
    assert(idle(p));
}

int main(void) {
    test_context();
    test_off();
    test_scope();
    test_map();
    test_fine_wheel();
    test_cap();
    test_close();
    puts("wheel zoom: context gating (screen ids, open state, missing site / manager / screens) and wheel-to-pulse "
         "checks (scope hold, map stick triggers, fine wheels, cap, closing) passed");
    return 0;
}
