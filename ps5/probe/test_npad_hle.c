// Host check of the game's controller boundary (runtime/hle/npad.c) as the PS5 title links it before M4: player 1 is a
// connected FullKey controller (the game never falls into the SDK's controller applet, whose hid shared memory no
// input service fills), its state what the input provider reads, with stick directions as buttons 16-23.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include "../../runtime/hle/npad.c"

pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
void klock(void) { pthread_mutex_lock(&g_klock); }
static int events, signals, attaches;
KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) { static KEvent ev; *w = 1, *r = 2; events++; return &ev; }
void kernel_signal(KObject *o) { (void)o; signals++; }
uint64_t guest_sym(const char *name) { assert(strstr(name, "AttachReadableHandleToSystemEvent")); return 0x7000001000; }
void guest_call(Cpu *c, uint64_t addr) { assert(addr == 0x7000001000 && c->x[1] == 2); attaches++; }
void ps5_logf(const char *format, ...) { (void)format; }

static NpadInput provided;
void input_npad_read(NpadInput *in, uint64_t id_addr) { (void)id_addr; *in = provided; }

static uint8_t state[8][0x28];
static void get_states(uint32_t id, int count) {
    Cpu c = {0};
    memset(state, 0xAA, sizeof state);
    c.x[0] = (uintptr_t)state, c.x[1] = (uint64_t)count, c.x[2] = (uintptr_t)&id;
    hid_states_fullkey(&c);
    assert(c.x[0] == (uint64_t)count);
}
static uint64_t buttons(int i) { return *(uint64_t *)(state[i] + 8); }
static uint32_t attributes(int i) { return *(uint32_t *)(state[i] + 32); }

int main(void) {
    Cpu c = {0};
    uint32_t id = 0;
    c.x[0] = (uintptr_t)&id;
    hid_style_set(&c);
    assert(c.x[0] == 1);  // player 1: FullKey
    id = 0x20;
    c.x[0] = (uintptr_t)&id;
    hid_style_set(&c);
    assert(c.x[0] == 0);  // Handheld: none (docked)

    // the PS5 before M4: nothing pressed, sticks at rest, connected
    get_states(0, 4);
    for (int i = 0; i < 4; i++) {
        assert(buttons(i) == 0 && attributes(i) == 1);
        assert(*(int64_t *)state[i] == *(int64_t *)state[0] - i);  // newest first
        for (int k = 16; k < 32; k++) assert(state[i][k] == 0);
    }
    get_states(0x20, 1);
    assert(buttons(0) == 0 && attributes(0) == 0);  // Handheld stays disconnected

    provided = (NpadInput){.buttons = 1, .lx = -32767, .ry = 20000};  // A, stick left, right stick up
    get_states(0, 1);
    assert(buttons(0) == (1ull | 1ull << 16 | 1ull << 21) && attributes(0) == 1);
    assert(*(int32_t *)(state[0] + 16) == -32767 && *(int32_t *)(state[0] + 28) == 20000);

    uint8_t event[0x30];
    c = (Cpu){0};
    c.x[0] = (uintptr_t)&id, c.x[1] = (uintptr_t)event, c.x[2] = 1;
    hid_bind_style_event(&c);
    assert(events == 1 && signals == 1 && attaches == 1);  // signaled once: the game sees its controller connected
    return 0;
}
