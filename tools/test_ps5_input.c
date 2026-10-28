// runtime/ps5/input.c against scripted scePad / sceUserService / sceKeyboard / sceMouse records, with no console or
// controller: the SDL gamepad it presents (buttons, axes, events), its lifecycle, the keyboards' keys as SDL scancodes,
// the mice's buttons and summed motion and wheel, and the scancode names. make test-ps5-input
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

// Keyboards and mice: index i opens as handle 20 + i (keyboard) / 30 + i (mouse) while hid_ok says so
static uint16_t sysmodules[4];
static int nsysmodules, kbd_open_calls[2], kbd_closes, mouse_closes;
static bool hid_ok[2][2] = {{true, false}, {true, true}};
static KeyboardRecord kq[2][KEYBOARD_RECORDS];
static MouseRecord mq[2][MOUSE_RECORDS];
static int32_t nk[2], nm[2];
int32_t sceSysmoduleLoadModule(uint16_t id) { assert(nsysmodules < 4); sysmodules[nsysmodules++] = id; return 0; }
int32_t sceKeyboardInit(void) { return 0; }
int32_t sceMouseInit(void) { return 0; }
int32_t sceKeyboardOpen(int32_t user, int32_t type, int32_t index, const void *params) {
    assert(type == 0 && index >= 0 && index < 2 && params);
    kbd_open_calls[index]++;
    return user == initial_user && hid_ok[0][index] ? 20 + index : -2;
}
int32_t sceMouseOpen(int32_t user, int32_t type, int32_t index, const void *params) {
    assert(type == 0 && index >= 0 && index < 2 && params);
    return user == initial_user && hid_ok[1][index] ? 30 + index : -2;
}
int32_t sceKeyboardClose(int32_t handle) { assert(handle == 20 || handle == 21); kbd_closes++; return 0; }
int32_t sceMouseClose(int32_t handle) { assert(handle == 30 || handle == 31); mouse_closes++; return 0; }
int32_t sceKeyboardRead(int32_t handle, void *records, int32_t capacity) {
    int i = handle - 20;
    assert((i == 0 || i == 1) && capacity == KEYBOARD_RECORDS);
    memcpy(records, kq[i], sizeof kq[i][0] * (size_t)nk[i]);
    int32_t n = nk[i];
    nk[i] = 0;
    return n;
}
int32_t sceMouseRead(int32_t handle, void *records, int32_t capacity) {
    int i = handle - 30;
    assert((i == 0 || i == 1) && capacity == MOUSE_RECORDS);
    memcpy(records, mq[i], sizeof mq[i][0] * (size_t)nm[i]);
    int32_t n = nm[i];
    nm[i] = 0;
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
static KeyboardRecord krec(uint32_t modifiers, uint16_t k0, uint16_t k1) {
    KeyboardRecord r;
    memset(&r, 0, sizeof r);
    r.connected = 1;
    r.length = 1;  // (also when nothing is held)
    r.modifiers = modifiers;
    r.keys[0] = k0, r.keys[1] = k1;
    r.timestamp_us = now_ns / 1000;
    return r;
}
static MouseRecord mrec(uint32_t buttons, int32_t x, int32_t y, int32_t wheel, int32_t tilt) {
    MouseRecord r;
    memset(&r, 0, sizeof r);
    r.connected = 1;
    r.buttons = buttons;
    r.x = x, r.y = y, r.wheel = wheel, r.tilt = tilt;
    r.timestamp_us = now_ns / 1000;
    return r;
}
static void key_records(int index, const KeyboardRecord *r, int n) {
    memcpy(kq[index], r, sizeof *r * (size_t)n);
    nk[index] = n;
}
static void mouse_records(int index, const MouseRecord *r, int n) {
    memcpy(mq[index], r, sizeof *r * (size_t)n);
    nm[index] = n;
}
static void poll_hid(void) { poll_with(NULL, 0); }
// The n-th key event's scancode and direction
static bool key_is(int i, SDL_Scancode sc, bool down) {
    return i < nevents && events[i].type == (down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP) &&
           events[i].key.scancode == sc && events[i].key.down == down;
}
static bool button_is(int i, int button, bool down) {
    return i < nevents && events[i].type == (down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP) &&
           events[i].button.button == button && events[i].button.down == down;
}

int main(void) {
    // Startup: the initial user's pad opens; no controller record yet -> no gamepad
    poll_with(NULL, 0);
    assert(opens == 1 && g_pad.handle == open_result && nevents == 0);
    // ... and the keyboard and mouse sysmodules load, their libraries start, and indexes 0 and 1 of each open
    // (keyboard index 1 is refused here: tried again each second)
    assert(nsysmodules == 2 && sysmodules[0] == SYSMODULE_KEYBOARD && sysmodules[1] == SYSMODULE_MOUSE);
    assert(g_hid.dev[HID_KEYBOARD][0].handle == 20 && g_hid.dev[HID_KEYBOARD][1].handle < 0 && kbd_open_calls[1] == 1);
    assert(g_hid.dev[HID_MOUSE][0].handle == 30 && g_hid.dev[HID_MOUSE][1].handle == 31);
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

    // ---- USB keyboards and mice. The logout closed the user's (index 0's keyboard, both mice), the login opened
    // them again; keyboard index 1, refused, is tried again only once a second has passed
    assert(kbd_closes == 1 && mouse_closes == 2 && nsysmodules == 2);
    assert(g_hid.dev[HID_KEYBOARD][0].handle == 20 && g_hid.dev[HID_MOUSE][0].handle == 30);
    poll_hid();
    int tries = kbd_open_calls[1];
    poll_hid();
    assert(kbd_open_calls[1] == tries && g_hid.dev[HID_KEYBOARD][1].handle < 0);
    hid_ok[0][1] = true;
    now_ns += 1000000000ull;
    poll_hid();
    assert(kbd_open_calls[1] == tries + 1 && g_hid.dev[HID_KEYBOARD][1].handle == 21 && nevents == 0);

    // Each HID usage is SDL's scancode for it, down then up, with SDL's key code (no modifier)
    static const struct { uint16_t usage; SDL_Scancode sc; SDL_Keycode key; } kmap[] = {
        {4, SDL_SCANCODE_A, 'a'},           {26, SDL_SCANCODE_W, 'w'},        {40, SDL_SCANCODE_RETURN, '\r'},
        {41, SDL_SCANCODE_ESCAPE, 0x1b},     {44, SDL_SCANCODE_SPACE, ' '},    {58, SDL_SCANCODE_F1, 0x4000003a},
        {82, SDL_SCANCODE_UP, 0x40000052},   {88, SDL_SCANCODE_KP_ENTER, 0x40000058},
    };
    for (size_t i = 0; i < sizeof kmap / sizeof kmap[0]; i++) {
        KeyboardRecord k = krec(0, kmap[i].usage, 0);
        key_records(0, &k, 1);
        poll_hid();
        assert(nevents == 1 && key_is(0, kmap[i].sc, true) && events[0].key.which == 1);
        assert(events[0].key.key == kmap[i].key && events[0].key.mod == SDL_KMOD_NONE && !events[0].key.repeat);
        k = krec(0, 0, 0);
        key_records(0, &k, 1);
        poll_hid();
        assert(nevents == 1 && key_is(0, kmap[i].sc, false));
    }
    // The modifier bits are Left Control..Right GUI: a chord's modifier goes down before its key, up after it, and
    // the key carries it (Shift + A is 'A')
    KeyboardRecord k2 = krec(1u << 1, 4, 0);
    key_records(0, &k2, 1);
    poll_hid();
    assert(nevents == 2 && key_is(0, SDL_SCANCODE_LSHIFT, true) && key_is(1, SDL_SCANCODE_A, true));
    assert(events[1].key.mod == SDL_KMOD_LSHIFT && events[1].key.key == 'A');
    k2 = krec(0, 0, 0);
    key_records(0, &k2, 1);
    poll_hid();
    assert(nevents == 2 && key_is(0, SDL_SCANCODE_A, false) && key_is(1, SDL_SCANCODE_LSHIFT, false));
    static const SDL_Scancode mods[8] = {SDL_SCANCODE_LCTRL, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LALT, SDL_SCANCODE_LGUI,
                                         SDL_SCANCODE_RCTRL, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RALT, SDL_SCANCODE_RGUI};
    for (unsigned b = 0; b < 8; b++) {
        k2 = krec(1u << b, 0, 0);
        key_records(0, &k2, 1);
        poll_hid();
        assert(nevents == 1 && key_is(0, mods[b], true));
        k2 = krec(0, 0, 0);
        key_records(0, &k2, 1);
        poll_hid();
        assert(nevents == 1 && key_is(0, mods[b], false));
    }
    k2 = krec(0xf0, 0, 0);  // (the right-hand ones together: SDL's modifier state)
    key_records(0, &k2, 1);
    poll_hid();
    assert(nevents == 4 && events[3].key.mod == (SDL_KMOD_RCTRL | SDL_KMOD_RSHIFT | SDL_KMOD_RALT | SDL_KMOD_RGUI));
    k2 = krec(0, 0, 0);
    key_records(0, &k2, 1);
    poll_hid();
    assert(nevents == 4 && count(SDL_EVENT_KEY_UP) == 4);

    // A batch is every record oldest-first: a tap between two polls is a press and a release; a rollover report
    // (usage 1) changes nothing; an empty read keeps what is held
    KeyboardRecord tap[3] = {krec(0, 4, 0), krec(0, 1, 1), krec(0, 4, 22)};
    key_records(0, tap, 3);
    poll_hid();
    assert(nevents == 2 && key_is(0, SDL_SCANCODE_A, true) && key_is(1, SDL_SCANCODE_S, true));
    poll_hid();
    assert(nevents == 0 && usage_held(g_hid.keys, SDL_SCANCODE_A) && usage_held(g_hid.keys, SDL_SCANCODE_S));
    KeyboardRecord up[2] = {krec(0, 0, 0), krec(0, 26, 0)};
    key_records(0, up, 2);
    poll_hid();
    assert(nevents == 3 && key_is(0, SDL_SCANCODE_A, false) && key_is(1, SDL_SCANCODE_S, false) &&
           key_is(2, SDL_SCANCODE_W, true));

    // Two keyboards: a key is down while either holds it; the second one's events name it (which 2)
    k2 = krec(0, 26, 0);
    key_records(1, &k2, 1);
    poll_hid();
    assert(nevents == 0 && g_hid.dev[HID_KEYBOARD][1].connected);
    k2 = krec(0, 0, 0);
    key_records(0, &k2, 1);
    poll_hid();
    assert(nevents == 0);
    key_records(1, &k2, 1);
    poll_hid();
    assert(nevents == 1 && key_is(0, SDL_SCANCODE_W, false) && events[0].key.which == 2);

    // Unplugged: a disconnected record releases what it held; replugged, the same handle reports again; a record
    // the system intercepts holds nothing
    k2 = krec(0, 4, 0);
    key_records(0, &k2, 1);
    poll_hid();
    assert(nevents == 1 && key_is(0, SDL_SCANCODE_A, true));
    k2.connected = 0;
    key_records(0, &k2, 1);
    poll_hid();
    assert(nevents == 1 && key_is(0, SDL_SCANCODE_A, false) && !g_hid.dev[HID_KEYBOARD][0].connected);
    k2 = krec(0, 4, 0);
    key_records(0, &k2, 1);
    poll_hid();
    assert(nevents == 1 && key_is(0, SDL_SCANCODE_A, true) && g_hid.dev[HID_KEYBOARD][0].connected);
    k2.intercepted = 1;
    key_records(0, &k2, 1);
    poll_hid();
    assert(nevents == 1 && key_is(0, SDL_SCANCODE_A, false) && g_hid.dev[HID_KEYBOARD][0].connected);

    // Mouse buttons: bits 0..4 are SDL's left, right, middle, X1, X2
    static const int bmap[5] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE, SDL_BUTTON_X1, SDL_BUTTON_X2};
    for (int b = 0; b < 5; b++) {
        MouseRecord m = mrec(1u << b, 0, 0, 0, 0);
        mouse_records(0, &m, 1);
        poll_hid();
        assert(nevents == 1 && button_is(0, bmap[b], true));
        m = mrec(0, 0, 0, 0, 0);
        mouse_records(0, &m, 1);
        poll_hid();
        assert(nevents == 1 && button_is(0, bmap[b], false));
    }
    // A click between two polls is a press and a release; an empty read keeps the button held
    MouseRecord click[3] = {mrec(1, 0, 0, 0, 0), mrec(0, 0, 0, 0, 0), mrec(2, 0, 0, 0, 0)};
    mouse_records(0, click, 3);
    poll_hid();
    assert(nevents == 3 && button_is(0, SDL_BUTTON_LEFT, true) && button_is(1, SDL_BUTTON_LEFT, false) &&
           button_is(2, SDL_BUTTON_RIGHT, true));
    poll_hid();
    assert(nevents == 0 && g_hid.buttons == 1u << SDL_BUTTON_RIGHT);

    // Motion and wheel: every usable record of every mouse in a poll, summed into one motion and one wheel event; an
    // intercepted or disconnected record adds nothing (and releases its buttons)
    MouseRecord m0[3] = {mrec(2, 3, -2, 1, 0), mrec(2, 5, 4, 0, -1), mrec(2 | MOUSE_INTERCEPTED, 100, 100, 9, 9)};
    MouseRecord m1[2] = {mrec(0, -1, 1, 2, 0), mrec(0, 50, 50, 5, 5)};
    m1[1].connected = 0;
    mouse_records(0, m0, 3);
    mouse_records(1, m1, 2);
    poll_hid();
    assert(count(SDL_EVENT_MOUSE_MOTION) == 1 && count(SDL_EVENT_MOUSE_WHEEL) == 1 && count(SDL_EVENT_MOUSE_BUTTON_UP) == 1);
    const SDL_Event *mv = find(SDL_EVENT_MOUSE_MOTION, 0), *wh = find(SDL_EVENT_MOUSE_WHEEL, 0);
    assert(mv->motion.xrel == 7 && mv->motion.yrel == 3 && wh->wheel.y == 3 && wh->wheel.x == -1);
    assert(button_is(0, SDL_BUTTON_RIGHT, false) && g_hid.buttons == 0 && nevents == 3);
    poll_hid();
    assert(nevents == 0);  // (no records: no motion)
    // Two mice: a button is down while either holds it
    MouseRecord l = mrec(1, 0, 0, 0, 0);
    mouse_records(0, &l, 1);
    mouse_records(1, &l, 1);
    poll_hid();
    assert(nevents == 1 && button_is(0, SDL_BUTTON_LEFT, true));
    l = mrec(0, 0, 0, 0, 0);
    mouse_records(0, &l, 1);
    poll_hid();
    assert(nevents == 0);

    // The user logs out while a key and a button are held: released, and every keyboard and mouse closed
    k2 = krec(0, 4, 0);
    key_records(0, &k2, 1);
    poll_hid();
    user_queue[nuser++] = (UserServiceEvent){1, initial_user};
    poll_hid();
    assert(count(SDL_EVENT_KEY_UP) == 1 && count(SDL_EVENT_MOUSE_BUTTON_UP) == 1 && g_hid.buttons == 0);
    assert(kbd_closes == 3 && mouse_closes == 4 && g_hid.dev[HID_KEYBOARD][0].handle < 0);
    printf("test_ps5_input: ok\n");
    return 0;
}
