// The game's controllers (nn::hid Npad) and their vibration devices: player 1 is a FullKey controller, always
// connected (docked: Handheld stays disconnected), its buttons and sticks read from the host's input devices
// (runtime/npad.h). With no device in hand it is a controller with nothing pressed. The SDK's own versions of these
// calls read the hid shared memory, which no input service fills here.
#include <string.h>

#include "../hle.h"
#include "../kernel.h"
#include "../npad.h"

static _Atomic int64_t sampling;

// NpadFullKeyState / NpadHandheldState / NpadJoy*State share this layout (0x28 bytes):
// {int64 sampling, u64 buttons, AnalogStick L {x,y}, AnalogStick R {x,y}, u32 attributes}
static void fill_state(Cpu *c) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[1];
    memset(s, 0, 0x28);
    int64_t n = ++sampling;
    *(int64_t *)s = n;
    static int nlog;
    if (nlog < 6 && (n % 600) == 1) { nlog++; LOG("[input] GetNpadState id %#x lr %llx\n", id, c->x[30]); }
    if (id != 0) return;  // docked: only player 1 (FullKey); Handheld stays disconnected
    NpadInput in = {0};
    input_npad_read(&in, c->x[1]);
    uint64_t b = in.buttons;
    // stick directions also appear as buttons (bits 16-23)
    if (in.lx < -16000) b |= 1ull << 16;
    if (in.ly > 16000) b |= 1ull << 17;
    if (in.lx > 16000) b |= 1ull << 18;
    if (in.ly < -16000) b |= 1ull << 19;
    if (in.rx < -16000) b |= 1ull << 20;
    if (in.ry > 16000) b |= 1ull << 21;
    if (in.rx > 16000) b |= 1ull << 22;
    if (in.ry < -16000) b |= 1ull << 23;
    *(uint64_t *)(s + 8) = b;
    static uint64_t last_b;
    if (b != last_b && getenv("SAVAGEWIND_INPUT")) LOG("[input] id %#x buttons %#llx (sampling %lld, lr %llx)\n", id, (unsigned long long)b, (long long)n, c->x[30]);
    last_b = b;
    *(int32_t *)(s + 16) = in.lx;
    *(int32_t *)(s + 20) = in.ly;
    *(int32_t *)(s + 24) = in.rx;
    *(int32_t *)(s + 28) = in.ry;
    *(uint32_t *)(s + 32) = 1;  // IsConnected
}

HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { fill_state(c); }

// int GetNpadStates(State *out, int count, const NpadIdType &id): the newest `count` samples, newest first (BOTW
// 1.8.2 reads its controllers only this way). Every sample is the current state, with sampling numbers counting
// back from the newest.
static void fill_states(Cpu *c) {
    uint8_t *out = (uint8_t *)(uintptr_t)c->x[0];
    int count = (int)c->x[1];
    uint64_t id_ptr = c->x[2];
    if (!out || count <= 0) { c->x[0] = 0; return; }
    uint64_t saved = c->x[1];
    c->x[1] = id_ptr;  // (fill_state's arguments: state, &id)
    fill_state(c);
    c->x[1] = saved;
    int64_t newest = *(int64_t *)out;
    for (int i = 1; i < count; i++) {
        memcpy(out + 0x28 * i, out, 0x28);
        *(int64_t *)(out + 0x28 * i) = newest - i;
    }
    c->x[0] = (uint64_t)count;
}
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", hid_states_fullkey) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", hid_states_dual) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", hid_states_left) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", hid_states_handheld) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", hid_states_right) { fill_states(c); }

// FullKey for player 1 only (we report docked mode, where Handheld is unavailable)
HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];
    static int nlog;
    if (nlog++ < 4) LOG("[input] GetNpadStyleSet id %#x\n", id);
    c->x[0] = id == 0 ? 1 : 0;  // docked: no handheld controller
}

// BindNpadStyleSetUpdateEvent(id, SystemEventType*, clear mode): signaled once so the game sees a
// controller connected.
HLE_FUNC("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", hid_bind_style_event) {
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    klock();
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    uint64_t clear_mode = c->x[2];
    c->x[0] = c->x[1];
    c->x[1] = r;
    c->x[2] = 0;
    c->x[3] = clear_mode;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    LOG("[input] style set update event bound\n");
}

// The SDK's vibration node graph stays recompiled; its mixed output reaches services/hid_set.c.
HLE_FUNC("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_init_vibration) { (void)c; }
HLE_FUNC("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    for (int i = 0; i < n; i++) out[i] = 3u | (id & 0xFF) << 8 | (uint32_t)i << 16;  // style, player, device, padding
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vibration_info) {
    uint32_t *info = (uint32_t *)(uintptr_t)c->x[0];
    uint32_t h = *(uint32_t *)(uintptr_t)c->x[1];
    info[0] = 1;        // VibrationDeviceType_LinearResonantActuator
    info[1] = 1 + ((h >> 16) & 1);  // position: left = 1, right = 2
}
