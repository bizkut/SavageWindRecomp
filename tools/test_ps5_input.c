// runtime/ps5/input.c against scripted scePad / sceUserService records, with no console or controller: the SDL
// gamepad it presents (buttons, axes, events), its lifecycle and the scancode names. make test-ps5-input
#include <assert.h>
#include <stdio.h>
#include "../runtime/ps5/input.c"
#include "../runtime/hle/bindings.c"

// ---- fakes: clock, errors, the event queue, the console's libraries
static uint64_t now_ns = 1000000000ull;
uint64_t plat_now_ns(void) { return now_ns; }
uint64_t SDL_GetTicks(void) { return now_ns / 1000000ull; }
static const char *last_error;
void ps5_sdl_error(const char *text) { last_error = text; }
int input_pad_family(void) { return 0; }  // (bindings.c's labels; runtime/hle/input.c is not linked here)

static SDL_Event events[512];
static int nevents;
bool SDL_PushEvent(SDL_Event *e) {
    assert(nevents < 512);
    events[nevents++] = *e;
    return true;
}

static int32_t initial_user = 0x10000001, open_result = 7, opens, closes;
static UserServiceEvent user_queue[4];
static int nuser;
int32_t sceUserServiceInitialize(const void *params) { (void)params; return 0; }
int32_t sceUserServiceGetInitialUser(int32_t *user) { *user = initial_user; return 0; }
int32_t sceUserServiceGetEvent(UserServiceEvent *ev) {
    if (!nuser) return (int32_t)USER_SERVICE_NO_EVENT;
    *ev = user_queue[0];
    memmove(user_queue, user_queue + 1, sizeof user_queue[0] * (size_t)--nuser);
    return 0;
}
int32_t scePadInit(void) { return 0; }
int32_t scePadOpen(int32_t user, int32_t type, int32_t index, const void *params) {
    (void)params;
    assert(type == 0 && index == 0);
    opens++;
    return user == initial_user ? open_result : -1;
}
int32_t scePadClose(int32_t handle) { assert(handle == open_result); closes++; return 0; }
static PadRecord batch[PAD_RECORDS];
static int32_t nbatch;
static int32_t motion_state = -1, tilt_state = -1, deadband_state = -1, resets, vib_calls, vib_result;
static PadVibration vib;
int32_t scePadSetMotionSensorState(int32_t h, bool on) { assert(h == open_result); motion_state = on; return 0; }
int32_t scePadSetTiltCorrectionState(int32_t h, bool on) { assert(h == open_result); tilt_state = on; return 0; }
int32_t scePadSetAngularVelocityDeadbandState(int32_t h, bool on) { assert(h == open_result); deadband_state = on; return 0; }
int32_t scePadResetOrientation(int32_t h) { assert(h == open_result); resets++; return 0; }
int32_t scePadReadState(int32_t h, void *record) { assert(h == open_result); memset(record, 0, sizeof(PadRecord)); return 0; }
int32_t scePadGetControllerInformation(int32_t h, PadInformation *info) {
    assert(h == open_result);
    memset(info, 0, sizeof *info);
    info->connected = 1;
    return 0;
}
int32_t scePadSetVibration(int32_t h, const PadVibration *v) {
    assert(h == open_result);
    vib_calls++;
    if (vib_result) return vib_result;
    vib = *v;
    return 0;
}
int32_t scePadRead(int32_t handle, void *records, int32_t capacity) {
    assert(handle == open_result && capacity == PAD_RECORDS);
    memcpy(records, batch, sizeof batch[0] * (size_t)nbatch);
    int32_t n = nbatch;
    nbatch = 0;
    return n;
}

// ---- helpers
static PadRecord rec(uint32_t buttons) {
    PadRecord r;
    memset(&r, 0, sizeof r);
    r.buttons = buttons;
    r.lx = r.ly = r.rx = r.ry = 128;
    r.orientation[3] = 1;
    r.connected = 1;
    r.connected_count = 1;
    r.timestamp_us = now_ns / 1000;
    return r;
}
static void poll_with(const PadRecord *r, int n) {
    memcpy(batch, r, sizeof *r * (size_t)n);
    nbatch = n;
    nevents = 0;
    now_ns += 4000000ull;
    ps5_input_poll();
}
static int count(uint32_t type) {
    int n = 0;
    for (int i = 0; i < nevents; i++) n += events[i].type == type;
    return n;
}
static const SDL_Event *find(uint32_t type, int which) {
    for (int i = 0; i < nevents; i++)
        if (events[i].type == type && which-- == 0) return &events[i];
    return NULL;
}

int main(void) {
    // Startup: the initial user's pad opens; no controller record yet -> no gamepad
    poll_with(NULL, 0);
    assert(opens == 1 && g_pad.handle == open_result && nevents == 0);
    int n = 5;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    assert(n == 0);
    SDL_free(ids);

    // A connected record adds the gamepad: an SDL PS5 pad, centred
    PadRecord r = rec(0);
    poll_with(&r, 1);
    // (then the sticks' first values: input.c reads a gamepad's state when an event comes)
    assert(count(SDL_EVENT_GAMEPAD_ADDED) == 1 && count(SDL_EVENT_GAMEPAD_AXIS_MOTION) == 4 && nevents == 5);
    assert(events[0].type == SDL_EVENT_GAMEPAD_ADDED);
    SDL_JoystickID id = find(SDL_EVENT_GAMEPAD_ADDED, 0)->gdevice.which;
    ids = SDL_GetGamepads(&n);
    assert(n == 1 && ids[0] == id);
    SDL_free(ids);
    SDL_Gamepad *gp = SDL_OpenGamepad(id);
    assert(gp && SDL_GetGamepadID(gp) == id && SDL_GetGamepadType(gp) == SDL_GAMEPAD_TYPE_PS5);
    assert(SDL_GetGamepadTypeForID(id) == SDL_GAMEPAD_TYPE_PS5 && !SDL_OpenGamepad(id + 1));
    assert(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) == 128 && SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) == 0);

    // Each DualSense button -> SDL's button for it (SDL's DualSense mapping), down then up
    static const struct { uint32_t bit; SDL_GamepadButton b; } map[] = {
        {PAD_CROSS, SDL_GAMEPAD_BUTTON_SOUTH}, {PAD_CIRCLE, SDL_GAMEPAD_BUTTON_EAST},
        {PAD_SQUARE, SDL_GAMEPAD_BUTTON_WEST}, {PAD_TRIANGLE, SDL_GAMEPAD_BUTTON_NORTH},
        {PAD_CREATE, SDL_GAMEPAD_BUTTON_BACK}, {PAD_OPTIONS, SDL_GAMEPAD_BUTTON_START},
        {PAD_L3, SDL_GAMEPAD_BUTTON_LEFT_STICK}, {PAD_R3, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {PAD_L1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {PAD_R1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {PAD_UP, SDL_GAMEPAD_BUTTON_DPAD_UP}, {PAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {PAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {PAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {PAD_TOUCHPAD, SDL_GAMEPAD_BUTTON_TOUCHPAD},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        r = rec(map[i].bit);
        poll_with(&r, 1);
        const SDL_Event *e = find(SDL_EVENT_GAMEPAD_BUTTON_DOWN, 0);
        assert(nevents == 1 && e && e->gbutton.button == map[i].b && e->gbutton.down && e->gbutton.which == id);
        assert(SDL_GetGamepadButton(gp, map[i].b));
        r = rec(0);
        poll_with(&r, 1);
        e = find(SDL_EVENT_GAMEPAD_BUTTON_UP, 0);
        assert(nevents == 1 && e && e->gbutton.button == map[i].b && !e->gbutton.down);
        assert(!SDL_GetGamepadButton(gp, map[i].b));
    }
    // L2 / R2's bits are not buttons: the triggers are axes (input.c presses ZL / ZR past half travel)
    r = rec(PAD_L2 | PAD_R2);
    r.l2 = 255, r.r2 = 128;
    poll_with(&r, 1);
    assert(count(SDL_EVENT_GAMEPAD_BUTTON_DOWN) == 0 && count(SDL_EVENT_GAMEPAD_AXIS_MOTION) == 2);
    assert(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) == 32767);
    assert(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) == 16448);

    // Sticks: SDL's scaling (0 -> -32768, 255 -> 32767), y down as SDL has it
    r = rec(0);
    r.lx = 0, r.ly = 255, r.rx = 255, r.ry = 0;
    poll_with(&r, 1);
    assert(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) == -32768 && SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY) == 32767);
    assert(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX) == 32767 && SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY) == -32768);
    assert(count(SDL_EVENT_GAMEPAD_AXIS_MOTION) == 6);  // (four sticks, both triggers back to 0)
    poll_with(&r, 1);
    assert(nevents == 0);  // (no change, no event)

    // A batch: drained oldest-first, the newest record is the current state (a press and release inside it is not)
    PadRecord two[2] = {rec(PAD_CROSS), rec(PAD_CIRCLE)};
    poll_with(two, 2);
    assert(!SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH) && SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST));
    poll_with(NULL, 0);
    assert(nevents == 0 && SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST));  // (an empty read keeps the state)

    // Motion: configured at the connection (sensors on, tilt correction and deadband off, orientation reset); each
    // record's readings as SDL sensor events once input.c turns them on, at the record's time, G -> m/s^2
    assert(motion_state == 1 && tilt_state == 0 && deadband_state == 0 && resets == 1);
    assert(SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(gp, SDL_SENSOR_ACCEL));
    PadRecord m3[3] = {rec(PAD_CIRCLE), rec(PAD_CIRCLE), rec(PAD_CIRCLE)};
    poll_with(m3, 3);
    assert(count(SDL_EVENT_GAMEPAD_SENSOR_UPDATE) == 0);  // (not turned on yet)
    assert(SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_ACCEL, true) && SDL_SetGamepadSensorEnabled(gp, SDL_SENSOR_GYRO, true));
    for (int i = 0; i < 3; i++) {
        m3[i].timestamp_us = 5000000 + 4000 * (uint64_t)i;
        m3[i].accel[1] = 1, m3[i].gyro[0] = 0.5f * (float)i, m3[i].gyro[2] = -1;
    }
    poll_with(m3, 3);
    assert(count(SDL_EVENT_GAMEPAD_SENSOR_UPDATE) == 6 && count(SDL_EVENT_GAMEPAD_BUTTON_DOWN) == 0);
    for (int i = 0; i < 3; i++) {
        const SDL_Event *a = &events[2 * i], *g = &events[2 * i + 1];
        assert(a->gsensor.sensor == SDL_SENSOR_ACCEL && g->gsensor.sensor == SDL_SENSOR_GYRO && a->gsensor.which == id);
        assert(a->gsensor.sensor_timestamp == (5000000 + 4000 * (uint64_t)i) * 1000 && g->gsensor.sensor_timestamp == a->gsensor.sensor_timestamp);
        assert(a->gsensor.data[0] == 0 && fabsf(a->gsensor.data[1] - 9.80665f) < 1e-4f && a->gsensor.data[2] == 0);
        assert(g->gsensor.data[0] == 0.5f * (float)i && g->gsensor.data[1] == 0 && g->gsensor.data[2] == -1);
    }

    // Rumble: SDL's speeds' high bytes to the large / small motors; held for the duration asked, then stopped
    assert(SDL_RumbleGamepad(gp, 0xffff, 0x8000, 100) && vib.large_motor == 255 && vib.small_motor == 128 && vib_calls == 1);
    now_ns += 50000000ull;
    assert(SDL_RumbleGamepad(gp, 0xffff, 0x8000, 100) && vib_calls == 1);  // (unchanged: the lease is renewed)
    poll_with(NULL, 0);
    assert(vib_calls == 1);
    now_ns += 200000000ull;
    poll_with(NULL, 0);
    assert(vib_calls == 2 && vib.large_motor == 0 && vib.small_motor == 0);
    vib_result = -5;
    assert(!SDL_RumbleGamepad(gp, 0x100, 0, 100) && strstr(last_error, "scePadSetVibration failed"));
    vib_result = 0;
    assert(SDL_RumbleGamepad(gp, 0x4000, 0, 0) && vib.large_motor == 0x40 && vib_calls == 4);

    // The system takes the controller: removed, nothing held, the motors stopped; the removed gamepad keeps its id
    // until input.c closes it. Back: a new gamepad, its sensors configured afresh
    r = rec(PAD_CIRCLE | PAD_INTERCEPTED);
    poll_with(&r, 1);
    assert(count(SDL_EVENT_GAMEPAD_REMOVED) == 1 && find(SDL_EVENT_GAMEPAD_REMOVED, 0)->gdevice.which == id);
    assert(SDL_GetGamepadID(gp) == id && vib.large_motor == 0 && vib_calls == 5);
    assert(!SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST) && SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) == 0);
    assert(!SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO) && SDL_RumbleGamepad(gp, 0, 0, 100) && !SDL_RumbleGamepad(gp, 1 << 15, 0, 100));
    SDL_CloseGamepad(gp);
    r = rec(0);
    poll_with(&r, 1);
    assert(count(SDL_EVENT_GAMEPAD_ADDED) == 1 && resets == 2);
    SDL_JoystickID id2 = find(SDL_EVENT_GAMEPAD_ADDED, 0)->gdevice.which;
    assert(id2 != id && !SDL_OpenGamepad(id) && (gp = SDL_OpenGamepad(id2)) && SDL_GetGamepadID(gp) == id2);
    m3[0] = rec(0);
    poll_with(m3, 1);
    assert(count(SDL_EVENT_GAMEPAD_SENSOR_UPDATE) == 0);  // (a new connection: off until input.c turns them on)

    // A new connection generation inside one batch: removed, then added again; the old gamepad reads nothing
    PadRecord gen[2] = {rec(0), rec(PAD_CROSS)};
    gen[1].connected_count = 2;
    poll_with(gen, 2);
    assert(count(SDL_EVENT_GAMEPAD_REMOVED) == 1 && count(SDL_EVENT_GAMEPAD_ADDED) == 1);
    assert(events[0].type == SDL_EVENT_GAMEPAD_REMOVED && events[1].type == SDL_EVENT_GAMEPAD_ADDED);
    assert(!SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH) && SDL_GetGamepadID(gp) == id2);
    SDL_CloseGamepad(gp);
    gp = SDL_OpenGamepad(find(SDL_EVENT_GAMEPAD_ADDED, 0)->gdevice.which);
    assert(gp && SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH));

    // Disconnected: removed
    r = rec(0);
    r.connected = 0;
    poll_with(&r, 1);
    assert(count(SDL_EVENT_GAMEPAD_REMOVED) == 1 && !SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH));
    ids = SDL_GetGamepads(&n);
    assert(n == 0);
    SDL_free(ids);
    SDL_CloseGamepad(gp);

    // The user logs out: the pad is closed; another user logs in: theirs opens
    r = rec(0);
    r.connected_count = 3;
    poll_with(&r, 1);
    assert(count(SDL_EVENT_GAMEPAD_ADDED) == 1);
    user_queue[nuser++] = (UserServiceEvent){1, initial_user};
    poll_with(NULL, 0);
    assert(closes == 1 && g_pad.handle < 0 && g_pad.user < 0 && count(SDL_EVENT_GAMEPAD_REMOVED) == 1);
    initial_user = 0x10000002;
    user_queue[nuser++] = (UserServiceEvent){0, initial_user};
    poll_with(NULL, 0);
    assert(g_pad.user == initial_user && g_pad.handle == open_result && opens == 2);

    // Polls closer than 2 ms apart read nothing (the window loop asks once per event)
    batch[0] = rec(0);
    nbatch = 1;
    nevents = 0;
    ps5_input_poll();
    assert(nbatch == 1 && nevents == 0);

    // Keyboard names: SDL's, and back (case-insensitive), for every named scancode
    assert(!strcmp(SDL_GetScancodeName(SDL_SCANCODE_LSHIFT), "Left Shift"));
    assert(!strcmp(SDL_GetScancodeName((SDL_Scancode)98), "Keypad 0"));
    assert(SDL_GetScancodeFromName("left shift") == SDL_SCANCODE_LSHIFT && SDL_GetScancodeFromName("") == 0);
    for (int i = 1; i < SDL_SCANCODE_COUNT; i++) {  // (some names repeat, as in SDL: the first one is found)
        const char *nm = SDL_GetScancodeName((SDL_Scancode)i);
        if (*nm) assert(!strcmp(SDL_GetScancodeName(SDL_GetScancodeFromName(nm)), nm));
    }
    SDL_Keymod mod;
    assert(SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_NONE, false) == 'a');
    assert(SDL_GetKeyFromScancode(SDL_SCANCODE_A, SDL_KMOD_LSHIFT, false) == 'A');
    assert(SDL_GetScancodeFromKey('A', &mod) == SDL_SCANCODE_A && mod == SDL_KMOD_LSHIFT);
    // savagewind.ini's keyboard bindings read as on desktop
    assert(bind_settings_load("bind_key_a", "Left Shift") && g_bind_key[BIND_A] == SDL_SCANCODE_LSHIFT);
    assert(!strcmp(bind_key_label(g_bind_key[BIND_A]), "Left Shift"));
    bind_reset(BIND_DEV_KEY);
    printf("test_ps5_input: ok\n");
    return 0;
}
