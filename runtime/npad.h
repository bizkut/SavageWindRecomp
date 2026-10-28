// Player 1's controller for the guest's nn::hid Npad calls (runtime/hle/npad.c), read from the host's input devices:
// runtime/hle/input.c on desktop, runtime/ps5/unavailable.c on the PS5 until its input (M4).
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t buttons;        // Switch button bits 0-15 (A, B, X, Y, LStick, RStick, L, R, ZL, ZR, Plus, Minus, d-pad)
    int32_t lx, ly, rx, ry;  // sticks, -32767..32767, y up
} NpadInput;

// The game's controller thread. id_addr: the guest's NpadIdType argument (player 1's).
void input_npad_read(NpadInput *in, uint64_t id_addr);
